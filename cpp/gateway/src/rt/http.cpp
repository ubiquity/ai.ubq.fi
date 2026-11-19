#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/limits.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uos::rt {
  namespace detail {
    // Bun's Body.Value: null, a buffer (bytes, or the JS string it was made
    // from), or a ReadableStream.
    struct BodyState {
      enum class Kind : std::uint8_t { none, buffer, stream };
      Kind kind = Kind::none;
      std::shared_ptr<const Bytes> bytes;
      // A body made from a JS string: text() returns it unchanged (lone
      // surrogates, a leading BOM) until a clone turns it into bytes.
      std::optional<std::string> text;
      // A buffer read by a body method (Bun's Used state).
      bool used = false;
      // The stream of a stream body, or the stream `.body` made from the buffer.
      std::optional<ByteStream> stream;
      // The content type for a body from URLSearchParams.
      std::string contentType;
    };

    struct ResponseState {
      int status = 200;
      std::string statusText;
      Headers headers;
      // Without init.headers Bun creates the Headers object on first access and
      // adds the body's content type only if the body is still an untouched buffer.
      bool lazyContentType = false;
      std::string url;
      bool redirected = false;
      bool errorType = false;
      BodyState body;
    };

    struct RequestState {
      std::string method = "GET";
      std::string url;
      Headers headers;
      bool lazyContentType = false;
      AbortSignal signal;
      std::string redirect = "follow";
      std::string cache = "default";
      BodyState body;
      explicit RequestState(AbortSignal signal_) : signal(std::move(signal_)) {}
    };
  }

  namespace {
    using detail::BodyState;

    template <class... F> struct Overloaded : F... {
      using F::operator()...;
    };
    template <class... F> Overloaded(F...) -> Overloaded<F...>;

    char asciiLower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

    std::string asciiLowercase(std::string_view text) {
      std::string out(text);
      for (char& c : out) c = asciiLower(c);
      return out;
    }

    bool equalsIgnoringAsciiCase(std::string_view left, std::string_view right) {
      if (left.size() != right.size()) return false;
      for (std::size_t index = 0; index < left.size(); ++index)
        if (asciiLower(left[index]) != asciiLower(right[index])) return false;
      return true;
    }

    bool containsIgnoringAsciiCase(std::string_view text, std::string_view needle) {
      if (needle.size() > text.size()) return false;
      for (std::size_t start = 0; start + needle.size() <= text.size(); ++start)
        if (equalsIgnoringAsciiCase(text.substr(start, needle.size()), needle)) return true;
      return false;
    }

    std::shared_ptr<const Bytes> emptyBytes() {
      static const auto empty = std::make_shared<const Bytes>();
      return empty;
    }

    std::shared_ptr<const Bytes> bytesOfText(std::string_view text) { return std::make_shared<const Bytes>(encodeUtf8(text)); }

    // Buffered bodies yield up to 16 KiB first, then up to 2 MiB at a time.
    ByteStream bufferStream(std::shared_ptr<const Bytes> bytes) {
      auto offset = std::make_shared<std::size_t>(0);
      UnderlyingSource<ByteChunk> source;
      source.pull = [bytes, offset](ReadableStreamDefaultController<ByteChunk> controller) {
        if (*offset >= bytes->size()) {
          controller.close();
          return;
        }
        const std::size_t limit = *offset == 0 ? std::size_t{16384} : std::size_t{2097152};
        const std::size_t size = std::min(limit, bytes->size() - *offset);
        const auto begin = bytes->begin() + static_cast<std::ptrdiff_t>(*offset);
        controller.enqueue(ByteChunk(begin, begin + static_cast<std::ptrdiff_t>(size)));
        *offset += size;
        if (*offset >= bytes->size()) controller.close();
      };
      return ByteStream(std::move(source));
    }

    // Bun's MimeType table as its multipart parser applies it (filename extension,
    // lowercased, to type), extracted from Bun 1.4.0: 1124 extensions.
    struct ExtensionType {
      std::string_view extension;
      std::string_view type;
    };
    constexpr ExtensionType kExtensionTypes[] = {
      {"123", "application/vnd.lotus-1-2-3"}, {"1km", "application/vnd.1000minds.decision-model+xml"}, {"3dml", "text/vnd.in3d.3dml"}, {"3ds", "image/x-3ds"},
      {"3g2", "video/3gpp2"}, {"3gp", "video/3gpp"}, {"3gpp", "video/3gpp"}, {"3mf", "model/3mf"}, {"7z", "application/x-7z-compressed"},
      {"aab", "application/x-authorware-bin"}, {"aac", "audio/x-aac"}, {"aam", "application/x-authorware-map"}, {"aas", "application/x-authorware-seg"},
      {"abw", "application/x-abiword"}, {"ac", "application/vnd.nokia.n-gage.ac+xml"}, {"acc", "application/vnd.americandynamics.acc"},
      {"ace", "application/x-ace-compressed"}, {"acu", "application/vnd.acucobol"}, {"adp", "audio/adpcm"}, {"aep", "application/vnd.audiograph"},
      {"afm", "application/x-font-type1"}, {"afp", "application/vnd.ibm.modcap"}, {"age", "application/vnd.age"}, {"ahead", "application/vnd.ahead.space"},
      {"ai", "application/postscript"}, {"aif", "audio/x-aiff"}, {"aifc", "audio/x-aiff"}, {"aiff", "audio/x-aiff"},
      {"air", "application/vnd.adobe.air-application-installer-package+zip"}, {"ait", "application/vnd.dvb.ait"}, {"ami", "application/vnd.amiga.ami"},
      {"amr", "audio/amr"}, {"apk", "application/vnd.android.package-archive"}, {"apng", "image/apng"}, {"application", "application/x-ms-application"},
      {"apr", "application/vnd.lotus-approach"}, {"arc", "application/x-freearc"}, {"arj", "application/x-arj"}, {"asc", "application/pgp-signature"},
      {"asf", "video/x-ms-asf"}, {"asm", "text/x-asm"}, {"aso", "application/vnd.accpac.simply.aso"}, {"asx", "video/x-ms-asf"},
      {"atc", "application/vnd.acucorp"}, {"atom", "application/atom+xml"}, {"atomcat", "application/atomcat+xml"},
      {"atomdeleted", "application/atomdeleted+xml"}, {"atomsvc", "application/atomsvc+xml"}, {"atx", "application/vnd.antix.game-component"},
      {"au", "audio/basic"}, {"avci", "image/avci"}, {"avcs", "image/avcs"}, {"avi", "video/x-msvideo"}, {"avif", "image/avif"},
      {"aw", "application/applixware"}, {"azf", "application/vnd.airzip.filesecure.azf"}, {"azs", "application/vnd.airzip.filesecure.azs"},
      {"azv", "image/vnd.airzip.accelerator.azv"}, {"azw", "application/vnd.amazon.ebook"}, {"b16", "image/vnd.pco.b16"}, {"bat", "application/x-msdownload"},
      {"bcpio", "application/x-bcpio"}, {"bdf", "application/x-font-bdf"}, {"bdm", "application/vnd.syncml.dm+wbxml"}, {"bdoc", "application/x-bdoc"},
      {"bed", "application/vnd.realvnc.bed"}, {"bh2", "application/vnd.fujitsu.oasysprs"}, {"bin", "application/octet-stream"}, {"blb", "application/x-blorb"},
      {"blorb", "application/x-blorb"}, {"bmi", "application/vnd.bmi"}, {"bmml", "application/vnd.balsamiq.bmml+xml"}, {"bmp", "image/x-ms-bmp"},
      {"book", "application/vnd.framemaker"}, {"box", "application/vnd.previewsystems.box"}, {"boz", "application/x-bzip2"},
      {"bpk", "application/octet-stream"}, {"bsp", "model/vnd.valve.source.compiled-map"}, {"btif", "image/prs.btif"}, {"buffer", "application/octet-stream"},
      {"bz", "application/x-bzip"}, {"bz2", "application/x-bzip2"}, {"c", "text/x-c"}, {"c4d", "application/vnd.clonk.c4group"},
      {"c4f", "application/vnd.clonk.c4group"}, {"c4g", "application/vnd.clonk.c4group"}, {"c4p", "application/vnd.clonk.c4group"},
      {"c4u", "application/vnd.clonk.c4group"}, {"cab", "application/vnd.ms-cab-compressed"}, {"caf", "audio/x-caf"}, {"cap", "application/vnd.tcpdump.pcap"},
      {"car", "application/vnd.curl.car"}, {"cat", "application/vnd.ms-pki.seccat"}, {"cb7", "application/x-cbr"}, {"cba", "application/x-cbr"},
      {"cbr", "application/x-cbr"}, {"cbt", "application/x-cbr"}, {"cbz", "application/x-cbr"}, {"cc", "text/x-c"}, {"cco", "application/x-cocoa"},
      {"cct", "application/x-director"}, {"ccxml", "application/ccxml+xml"}, {"cdf", "application/x-netcdf"}, {"cdfx", "application/cdfx+xml"},
      {"cdkey", "application/vnd.mediastation.cdkey"}, {"cdx", "chemical/x-cdx"}, {"cdy", "application/vnd.cinderella"}, {"cer", "application/pkix-cert"},
      {"cfs", "application/x-cfs-compressed"}, {"cgm", "image/cgm"}, {"chat", "application/x-chat"}, {"chm", "application/vnd.ms-htmlhelp"},
      {"chrt", "application/vnd.kde.kchart"}, {"cif", "chemical/x-cif"}, {"cii", "application/vnd.anser-web-certificate-issue-initiation"},
      {"cil", "application/vnd.ms-artgalry"}, {"cjs", "text/javascript;charset=utf-8"}, {"cla", "application/vnd.claymore"}, {"class", "application/java-vm"},
      {"clkk", "application/vnd.crick.clicker.keyboard"}, {"clkp", "application/vnd.crick.clicker.palette"},
      {"clkt", "application/vnd.crick.clicker.template"}, {"clkw", "application/vnd.crick.clicker.wordbank"}, {"clkx", "application/vnd.crick.clicker"},
      {"clp", "application/x-msclip"}, {"cmc", "application/vnd.cosmocaller"}, {"cmdf", "chemical/x-cmdf"}, {"cml", "chemical/x-cml"},
      {"cmp", "application/vnd.yellowriver-custom-menu"}, {"cmx", "image/x-cmx"}, {"cod", "application/vnd.rim.cod"}, {"coffee", "text/coffeescript"},
      {"com", "application/x-msdownload"}, {"conf", "text/plain;charset=utf-8"}, {"cpio", "application/x-cpio"}, {"cpl", "application/cpl+xml"},
      {"cpp", "text/x-c"}, {"cpt", "application/mac-compactpro"}, {"crd", "application/x-mscardfile"}, {"crl", "application/pkix-crl"},
      {"crt", "application/x-x509-ca-cert"}, {"crx", "application/x-chrome-extension"}, {"cryptonote", "application/vnd.rig.cryptonote"},
      {"csh", "application/x-csh"}, {"csl", "application/vnd.citationstyles.style+xml"}, {"csml", "chemical/x-csml"}, {"csp", "application/vnd.commonspace"},
      {"css", "text/css;charset=utf-8"}, {"cst", "application/x-director"}, {"csv", "text/csv"}, {"cts", "text/javascript;charset=utf-8"},
      {"cu", "application/cu-seeme"}, {"curl", "text/vnd.curl"}, {"cww", "application/prs.cww"}, {"cxt", "application/x-director"}, {"cxx", "text/x-c"},
      {"dae", "model/vnd.collada+xml"}, {"daf", "application/vnd.mobius.daf"}, {"dart", "application/vnd.dart"}, {"davmount", "application/davmount+xml"},
      {"dbf", "application/vnd.dbf"}, {"dbk", "application/docbook+xml"}, {"dcr", "application/x-director"}, {"dcurl", "text/vnd.curl.dcurl"},
      {"dd2", "application/vnd.oma.dd2+xml"}, {"ddd", "application/vnd.fujixerox.ddd"}, {"ddf", "application/vnd.syncml.dmddf+xml"},
      {"dds", "image/vnd.ms-dds"}, {"deb", "application/x-debian-package"}, {"def", "text/plain;charset=utf-8"}, {"deploy", "application/octet-stream"},
      {"der", "application/x-x509-ca-cert"}, {"dfac", "application/vnd.dreamfactory"}, {"dgc", "application/x-dgc-compressed"}, {"dic", "text/x-c"},
      {"dir", "application/x-director"}, {"dis", "application/vnd.mobius.dis"}, {"disposition-n", "message/disposition-notification"},
      {"dist", "application/octet-stream"}, {"djv", "image/vnd.djvu"}, {"djvu", "image/vnd.djvu"}, {"dll", "application/x-msdownload"},
      {"dmg", "application/x-apple-diskimage"}, {"dmp", "application/vnd.tcpdump.pcap"}, {"dms", "application/octet-stream"}, {"dna", "application/vnd.dna"},
      {"doc", "application/msword"}, {"docm", "application/vnd.ms-word.document.macroenabled.12"},
      {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"}, {"dot", "application/msword"},
      {"dotm", "application/vnd.ms-word.template.macroenabled.12"}, {"dotx", "application/vnd.openxmlformats-officedocument.wordprocessingml.template"},
      {"dp", "application/vnd.osgi.dp"}, {"dpg", "application/vnd.dpgraph"}, {"dra", "audio/vnd.dra"}, {"drle", "image/dicom-rle"},
      {"dsc", "text/prs.lines.tag"}, {"dssc", "application/dssc+der"}, {"dtb", "application/x-dtbook+xml"}, {"dtd", "application/xml-dtd"},
      {"dts", "audio/vnd.dts"}, {"dump", "application/octet-stream"}, {"dvb", "video/vnd.dvb.file"}, {"dvi", "application/x-dvi"},
      {"dwd", "application/atsc-dwd+xml"}, {"dwf", "model/vnd.dwf"}, {"dwg", "image/vnd.dwg"}, {"dxf", "image/vnd.dxf"},
      {"dxp", "application/vnd.spotfire.dxp"}, {"dxr", "application/x-director"}, {"ear", "application/java-archive"},
      {"ecelp4800", "audio/vnd.nuera.ecelp4800"}, {"ecelp7470", "audio/vnd.nuera.ecelp7470"}, {"ecelp9600", "audio/vnd.nuera.ecelp9600"},
      {"ecma", "application/ecmascript"}, {"edm", "application/vnd.novadigm.edm"}, {"edx", "application/vnd.novadigm.edx"}, {"efif", "application/vnd.picsel"},
      {"ei6", "application/vnd.pg.osasli"}, {"elc", "application/octet-stream"}, {"emf", "image/emf"}, {"eml", "message/rfc822"},
      {"emma", "application/emma+xml"}, {"emotionml", "application/emotionml+xml"}, {"emz", "application/x-msmetafile"}, {"eol", "audio/vnd.digital-winds"},
      {"eot", "application/vnd.ms-fontobject"}, {"eps", "application/postscript"}, {"epub", "application/epub+zip"}, {"es", "application/ecmascript"},
      {"es3", "application/vnd.eszigno3+xml"}, {"esa", "application/vnd.osgi.subsystem"}, {"esf", "application/vnd.epson.esf"},
      {"et3", "application/vnd.eszigno3+xml"}, {"etx", "text/x-setext"}, {"eva", "application/x-eva"}, {"evy", "application/x-envoy"},
      {"exe", "application/x-msdownload"}, {"exi", "application/exi"}, {"exp", "application/express"}, {"exr", "image/aces"},
      {"ext", "application/vnd.novadigm.ext"}, {"ez", "application/andrew-inset"}, {"ez2", "application/vnd.ezpix-album"},
      {"ez3", "application/vnd.ezpix-package"}, {"f", "text/x-fortran"}, {"f4v", "video/x-f4v"}, {"f77", "text/x-fortran"}, {"f90", "text/x-fortran"},
      {"fbs", "image/vnd.fastbidsheet"}, {"fcdt", "application/vnd.adobe.formscentral.fcdt"}, {"fcs", "application/vnd.isac.fcs"},
      {"fdf", "application/vnd.fdf"}, {"fdt", "application/fdt+xml"}, {"fg5", "application/vnd.fujitsu.oasysgp"}, {"fgd", "application/x-director"},
      {"fh", "image/x-freehand"}, {"fh4", "image/x-freehand"}, {"fh5", "image/x-freehand"}, {"fh7", "image/x-freehand"}, {"fhc", "image/x-freehand"},
      {"fig", "application/x-xfig"}, {"fits", "image/fits"}, {"flac", "audio/x-flac"}, {"fli", "video/x-fli"}, {"flo", "application/vnd.micrografx.flo"},
      {"flv", "video/x-flv"}, {"flw", "application/vnd.kde.kivio"}, {"flx", "text/vnd.fmi.flexstor"}, {"fly", "text/vnd.fly"},
      {"fm", "application/vnd.framemaker"}, {"fnc", "application/vnd.frogans.fnc"}, {"fo", "application/vnd.software602.filler.form+xml"},
      {"for", "text/x-fortran"}, {"fpx", "image/vnd.fpx"}, {"frame", "application/vnd.framemaker"}, {"fsc", "application/vnd.fsc.weblaunch"},
      {"fst", "image/vnd.fst"}, {"ftc", "application/vnd.fluxtime.clip"}, {"fti", "application/vnd.anser-web-funds-transfer-initiation"},
      {"fvt", "video/vnd.fvt"}, {"fxp", "application/vnd.adobe.fxp"}, {"fxpl", "application/vnd.adobe.fxp"}, {"fzs", "application/vnd.fuzzysheet"},
      {"g2w", "application/vnd.geoplan"}, {"g3", "image/g3fax"}, {"g3w", "application/vnd.geospace"}, {"gac", "application/vnd.groove-account"},
      {"gam", "application/x-tads"}, {"gbr", "application/rpki-ghostbusters"}, {"gca", "application/x-gca-compressed"}, {"gdl", "model/vnd.gdl"},
      {"gdoc", "application/vnd.google-apps.document"}, {"ged", "text/vnd.familysearch.gedcom"}, {"geo", "application/vnd.dynageo"},
      {"gex", "application/vnd.geometry-explorer"}, {"ggb", "application/vnd.geogebra.file"}, {"ggt", "application/vnd.geogebra.tool"},
      {"ghf", "application/vnd.groove-help"}, {"gif", "image/gif"}, {"gim", "application/vnd.groove-identity-message"}, {"glb", "model/gltf-binary"},
      {"gltf", "model/gltf+json"}, {"gml", "application/gml+xml"}, {"gmx", "application/vnd.gmx"}, {"gnumeric", "application/x-gnumeric"},
      {"gph", "application/vnd.flographit"}, {"gpx", "application/gpx+xml"}, {"gqf", "application/vnd.grafeq"}, {"gqs", "application/vnd.grafeq"},
      {"gram", "application/srgs"}, {"gramps", "application/x-gramps-xml"}, {"gre", "application/vnd.geometry-explorer"},
      {"grv", "application/vnd.groove-injector"}, {"gsf", "application/x-font-ghostscript"}, {"gsheet", "application/vnd.google-apps.spreadsheet"},
      {"gtar", "application/x-gtar"}, {"gtm", "application/vnd.groove-tool-message"}, {"gtw", "model/vnd.gtw"}, {"gv", "text/vnd.graphviz"},
      {"gxf", "application/gxf"}, {"gxt", "application/vnd.geonext"}, {"gz", "application/gzip"}, {"h", "text/x-c"}, {"h261", "video/h261"},
      {"h263", "video/h263"}, {"h264", "video/h264"}, {"hal", "application/vnd.hal+xml"}, {"hbci", "application/vnd.hbci"},
      {"hbs", "text/x-handlebars-template"}, {"hdd", "application/x-virtualbox-hdd"}, {"hdf", "application/x-hdf"}, {"heic", "image/heic"},
      {"heif", "image/heif"}, {"hej2", "image/hej2k"}, {"held", "application/atsc-held+xml"}, {"hh", "text/x-c"}, {"hjson", "application/hjson"},
      {"hlp", "application/winhlp"}, {"hpgl", "application/vnd.hp-hpgl"}, {"hpid", "application/vnd.hp-hpid"}, {"hps", "application/vnd.hp-hps"},
      {"hqx", "application/mac-binhex40"}, {"hsj2", "image/hsj2"}, {"htc", "text/x-component"}, {"htke", "application/vnd.kenameaapp"},
      {"htm", "text/html;charset=utf-8"}, {"html", "text/html;charset=utf-8"}, {"hvd", "application/vnd.yamaha.hv-dic"},
      {"hvp", "application/vnd.yamaha.hv-voice"}, {"hvs", "application/vnd.yamaha.hv-script"}, {"i2g", "application/vnd.intergeo"},
      {"icc", "application/vnd.iccprofile"}, {"ice", "x-conference/x-cooltalk"}, {"icm", "application/vnd.iccprofile"}, {"ico", "image/x-icon"},
      {"ics", "text/calendar"}, {"ief", "image/ief"}, {"ifb", "text/calendar"}, {"ifm", "application/vnd.shana.informed.formdata"}, {"iges", "model/iges"},
      {"igl", "application/vnd.igloader"}, {"igm", "application/vnd.insors.igm"}, {"igs", "model/iges"}, {"igx", "application/vnd.micrografx.igx"},
      {"iif", "application/vnd.shana.informed.interchange"}, {"img", "application/octet-stream"}, {"imp", "application/vnd.accpac.simply.imp"},
      {"ims", "application/vnd.ms-ims"}, {"in", "text/plain;charset=utf-8"}, {"ini", "text/plain;charset=utf-8"}, {"ink", "application/inkml+xml"},
      {"inkml", "application/inkml+xml"}, {"install", "application/x-install-instructions"}, {"iota", "application/vnd.astraea-software.iota"},
      {"ipfix", "application/ipfix"}, {"ipk", "application/vnd.shana.informed.package"}, {"irm", "application/vnd.ibm.rights-management"},
      {"irp", "application/vnd.irepository.package+xml"}, {"iso", "application/x-iso9660-image"}, {"itp", "application/vnd.shana.informed.formtemplate"},
      {"its", "application/its+xml"}, {"ivp", "application/vnd.immervision-ivp"}, {"ivu", "application/vnd.immervision-ivu"},
      {"jad", "text/vnd.sun.j2me.app-descriptor"}, {"jade", "text/jade"}, {"jam", "application/vnd.jam"}, {"jar", "application/java-archive"},
      {"java", "text/x-java-source"}, {"jhc", "image/jphc"}, {"jisp", "application/vnd.jisp"}, {"jls", "image/jls"}, {"jlt", "application/vnd.hp-jlyt"},
      {"jng", "image/x-jng"}, {"jnlp", "application/x-java-jnlp-file"}, {"joda", "application/vnd.joost.joda-archive"}, {"jp2", "image/jp2"},
      {"jpe", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"jpf", "image/jpx"}, {"jpg", "image/jpeg"}, {"jpg2", "image/jp2"}, {"jpgm", "video/jpm"},
      {"jpgv", "video/jpeg"}, {"jph", "image/jph"}, {"jpm", "video/jpm"}, {"jpx", "image/jpx"}, {"js", "text/javascript;charset=utf-8"},
      {"json", "application/json;charset=utf-8"}, {"json5", "application/json5"}, {"jsonml", "application/jsonml+json"},
      {"jsx", "text/javascript;charset=utf-8"}, {"jxr", "image/jxr"}, {"jxra", "image/jxra"}, {"jxrs", "image/jxrs"}, {"jxs", "image/jxs"},
      {"jxsc", "image/jxsc"}, {"jxsi", "image/jxsi"}, {"jxss", "image/jxss"}, {"kar", "audio/midi"}, {"karbon", "application/vnd.kde.karbon"},
      {"kdbx", "application/x-keepass2"}, {"key", "application/x-iwork-keynote-sffkey"}, {"kfo", "application/vnd.kde.kformula"},
      {"kia", "application/vnd.kidspiration"}, {"kml", "application/vnd.google-earth.kml+xml"}, {"kmz", "application/vnd.google-earth.kmz"},
      {"kne", "application/vnd.kinar"}, {"knp", "application/vnd.kinar"}, {"kon", "application/vnd.kde.kontour"}, {"kpr", "application/vnd.kde.kpresenter"},
      {"kpt", "application/vnd.kde.kpresenter"}, {"kpxx", "application/vnd.ds-keypoint"}, {"ksp", "application/vnd.kde.kspread"},
      {"ktr", "application/vnd.kahootz"}, {"ktx", "image/ktx"}, {"ktx2", "image/ktx2"}, {"ktz", "application/vnd.kahootz"},
      {"kwd", "application/vnd.kde.kword"}, {"kwt", "application/vnd.kde.kword"}, {"latex", "application/x-latex"},
      {"lbd", "application/vnd.llamagraphics.life-balance.desktop"}, {"lbe", "application/vnd.llamagraphics.life-balance.exchange+xml"},
      {"les", "application/vnd.hhe.lesson-player"}, {"less", "text/less"}, {"lgr", "application/lgr+xml"}, {"lha", "application/x-lzh-compressed"},
      {"link66", "application/vnd.route66.link66+xml"}, {"list", "text/plain;charset=utf-8"}, {"lnk", "application/x-ms-shortcut"},
      {"log", "text/plain;charset=utf-8"}, {"lrf", "application/octet-stream"}, {"lrm", "application/vnd.ms-lrm"}, {"ltf", "application/vnd.frogans.ltf"},
      {"lua", "text/x-lua"}, {"luac", "application/x-lua-bytecode"}, {"lvp", "audio/vnd.lucent.voice"}, {"lwp", "application/vnd.lotus-wordpro"},
      {"lzh", "application/x-lzh-compressed"}, {"m13", "application/x-msmediaview"}, {"m14", "application/x-msmediaview"}, {"m1v", "video/mpeg"},
      {"m21", "application/mp21"}, {"m2a", "audio/mpeg"}, {"m2v", "video/mpeg"}, {"m3a", "audio/mpeg"}, {"m3u", "audio/x-mpegurl"},
      {"m3u8", "application/vnd.apple.mpegurl"}, {"m4a", "audio/x-m4a"}, {"m4p", "application/mp4"}, {"m4s", "video/iso.segment"},
      {"m4u", "video/vnd.mpegurl"}, {"m4v", "video/x-m4v"}, {"ma", "application/mathematica"}, {"mads", "application/mads+xml"},
      {"maei", "application/mmt-aei+xml"}, {"mag", "application/vnd.ecowin.chart"}, {"maker", "application/vnd.framemaker"}, {"man", "text/troff"},
      {"manifest", "text/cache-manifest"}, {"map", "application/json;charset=utf-8"}, {"mar", "application/octet-stream"}, {"markdown", "text/markdown"},
      {"mathml", "application/mathml+xml"}, {"mb", "application/mathematica"}, {"mbk", "application/vnd.mobius.mbk"}, {"mbox", "application/mbox"},
      {"mc1", "application/vnd.medcalcdata"}, {"mcd", "application/vnd.mcd"}, {"mcurl", "text/vnd.curl.mcurl"}, {"md", "text/markdown"},
      {"mdb", "application/x-msaccess"}, {"mdi", "image/vnd.ms-modi"}, {"mdx", "text/mdx"}, {"me", "text/troff"}, {"mesh", "model/mesh"},
      {"metalink", "application/metalink+xml"}, {"mets", "application/mets+xml"}, {"mfm", "application/vnd.mfmp"}, {"mft", "application/rpki-manifest"},
      {"mgp", "application/vnd.osgeo.mapguide.package"}, {"mgz", "application/vnd.proteus.magazine"}, {"mid", "audio/midi"}, {"midi", "audio/midi"},
      {"mie", "application/x-mie"}, {"mif", "application/vnd.mif"}, {"mime", "message/rfc822"}, {"mj2", "video/mj2"}, {"mjp2", "video/mj2"},
      {"mjs", "text/javascript;charset=utf-8"}, {"mk3d", "video/x-matroska"}, {"mka", "audio/x-matroska"}, {"mkd", "text/x-markdown"},
      {"mks", "video/x-matroska"}, {"mkv", "video/x-matroska"}, {"mlp", "application/vnd.dolby.mlp"}, {"mmd", "application/vnd.chipnuts.karaoke-mmd"},
      {"mmf", "application/vnd.smaf"}, {"mml", "text/mathml"}, {"mmr", "image/vnd.fujixerox.edmics-mmr"}, {"mng", "video/x-mng"},
      {"mny", "application/x-msmoney"}, {"mobi", "application/x-mobipocket-ebook"}, {"mods", "application/mods+xml"}, {"mov", "video/quicktime"},
      {"movie", "video/x-sgi-movie"}, {"mp2", "audio/mpeg"}, {"mp21", "application/mp21"}, {"mp2a", "audio/mpeg"}, {"mp3", "audio/mpeg"}, {"mp4", "video/mp4"},
      {"mp4a", "audio/mp4"}, {"mp4s", "application/mp4"}, {"mp4v", "video/mp4"}, {"mpc", "application/vnd.mophun.certificate"},
      {"mpd", "application/dash+xml"}, {"mpe", "video/mpeg"}, {"mpeg", "video/mpeg"}, {"mpf", "application/media-policy-dataset+xml"}, {"mpg", "video/mpeg"},
      {"mpg4", "video/mp4"}, {"mpga", "audio/mpeg"}, {"mpkg", "application/vnd.apple.installer+xml"}, {"mpm", "application/vnd.blueice.multipass"},
      {"mpn", "application/vnd.mophun.application"}, {"mpp", "application/vnd.ms-project"}, {"mpt", "application/vnd.ms-project"},
      {"mpy", "application/vnd.ibm.minipay"}, {"mqy", "application/vnd.mobius.mqy"}, {"mrc", "application/marc"}, {"mrcx", "application/marcxml+xml"},
      {"ms", "text/troff"}, {"mseed", "application/vnd.fdsn.mseed"}, {"mseq", "application/vnd.mseq"}, {"msf", "application/vnd.epson.msf"},
      {"msg", "application/vnd.ms-outlook"}, {"msh", "model/mesh"}, {"msi", "application/x-msdownload"}, {"msl", "application/vnd.mobius.msl"},
      {"msm", "application/octet-stream"}, {"msp", "application/octet-stream"}, {"msty", "application/vnd.muvee.style"}, {"mtl", "model/mtl"},
      {"mts", "text/javascript;charset=utf-8"}, {"mtsx", "text/javascript;charset=utf-8"}, {"mus", "application/vnd.musician"},
      {"musd", "application/mmt-usd+xml"}, {"musicxml", "application/vnd.recordare.musicxml+xml"}, {"mvb", "application/x-msmediaview"},
      {"mvt", "application/vnd.mapbox-vector-tile"}, {"mwf", "application/vnd.mfer"}, {"mxf", "application/mxf"},
      {"mxl", "application/vnd.recordare.musicxml"}, {"mxmf", "audio/mobile-xmf"}, {"mxml", "application/xv+xml"}, {"mxs", "application/vnd.triscape.mxs"},
      {"mxu", "video/vnd.mpegurl"}, {"n3", "text/n3"}, {"nb", "application/mathematica"}, {"nbp", "application/vnd.wolfram.player"},
      {"nc", "application/x-netcdf"}, {"ncx", "application/x-dtbncx+xml"}, {"nfo", "text/x-nfo"}, {"nitf", "application/vnd.nitf"},
      {"nlu", "application/vnd.neurolanguage.nlu"}, {"nml", "application/vnd.enliven"}, {"nnd", "application/vnd.noblenet-directory"},
      {"nns", "application/vnd.noblenet-sealer"}, {"nnw", "application/vnd.noblenet-web"}, {"npx", "image/vnd.net-fpx"}, {"nq", "application/n-quads"},
      {"nsc", "application/x-conference"}, {"nsf", "application/vnd.lotus-notes"}, {"nt", "application/n-triples"}, {"ntf", "application/vnd.nitf"},
      {"numbers", "application/x-iwork-numbers-sffnumbers"}, {"nzb", "application/x-nzb"}, {"oa2", "application/vnd.fujitsu.oasys2"},
      {"oa3", "application/vnd.fujitsu.oasys3"}, {"oas", "application/vnd.fujitsu.oasys"}, {"obd", "application/x-msbinder"},
      {"obgx", "application/vnd.openblox.game+xml"}, {"obj", "model/obj"}, {"oda", "application/oda"}, {"odb", "application/vnd.oasis.opendocument.database"},
      {"odc", "application/vnd.oasis.opendocument.chart"}, {"odf", "application/vnd.oasis.opendocument.formula"},
      {"odft", "application/vnd.oasis.opendocument.formula-template"}, {"odg", "application/vnd.oasis.opendocument.graphics"},
      {"odi", "application/vnd.oasis.opendocument.image"}, {"odm", "application/vnd.oasis.opendocument.text-master"},
      {"odp", "application/vnd.oasis.opendocument.presentation"}, {"ods", "application/vnd.oasis.opendocument.spreadsheet"},
      {"odt", "application/vnd.oasis.opendocument.text"}, {"oga", "audio/ogg"}, {"ogex", "model/vnd.opengex"}, {"ogg", "audio/ogg"}, {"ogv", "video/ogg"},
      {"ogx", "application/ogg"}, {"omdoc", "application/omdoc+xml"}, {"opf", "application/oebps-package+xml"}, {"opml", "text/x-opml"},
      {"oprc", "application/vnd.palm"}, {"opus", "audio/ogg"}, {"org", "text/x-org"}, {"osf", "application/vnd.yamaha.openscoreformat"},
      {"osfpvg", "application/vnd.yamaha.openscoreformat.osfpvg+xml"}, {"osm", "application/vnd.openstreetmap.data+xml"},
      {"otc", "application/vnd.oasis.opendocument.chart-template"}, {"otf", "font/otf"}, {"otg", "application/vnd.oasis.opendocument.graphics-template"},
      {"oth", "application/vnd.oasis.opendocument.text-web"}, {"oti", "application/vnd.oasis.opendocument.image-template"},
      {"otp", "application/vnd.oasis.opendocument.presentation-template"}, {"ots", "application/vnd.oasis.opendocument.spreadsheet-template"},
      {"ott", "application/vnd.oasis.opendocument.text-template"}, {"ova", "application/x-virtualbox-ova"}, {"ovf", "application/x-virtualbox-ovf"},
      {"owl", "application/rdf+xml"}, {"oxps", "application/oxps"}, {"oxt", "application/vnd.openofficeorg.extension"}, {"p", "text/x-pascal"},
      {"p10", "application/pkcs10"}, {"p12", "application/x-pkcs12"}, {"p7b", "application/x-pkcs7-certificates"}, {"p7c", "application/pkcs7-mime"},
      {"p7m", "application/pkcs7-mime"}, {"p7r", "application/x-pkcs7-certreqresp"}, {"p7s", "application/pkcs7-signature"}, {"p8", "application/pkcs8"},
      {"pac", "application/x-ns-proxy-autoconfig"}, {"pages", "application/x-iwork-pages-sffpages"}, {"pas", "text/x-pascal"},
      {"paw", "application/vnd.pawaafile"}, {"pbd", "application/vnd.powerbuilder6"}, {"pbm", "image/x-portable-bitmap"},
      {"pcap", "application/vnd.tcpdump.pcap"}, {"pcf", "application/x-font-pcf"}, {"pcl", "application/vnd.hp-pcl"}, {"pclxl", "application/vnd.hp-pclxl"},
      {"pct", "image/x-pict"}, {"pcurl", "application/vnd.curl.pcurl"}, {"pcx", "image/x-pcx"}, {"pdb", "application/x-pilot"}, {"pde", "text/x-processing"},
      {"pdf", "application/pdf"}, {"pem", "application/x-x509-ca-cert"}, {"pfa", "application/x-font-type1"}, {"pfb", "application/x-font-type1"},
      {"pfm", "application/x-font-type1"}, {"pfr", "application/font-tdpfr"}, {"pfx", "application/x-pkcs12"}, {"pgm", "image/x-portable-graymap"},
      {"pgn", "application/x-chess-pgn"}, {"pgp", "application/pgp-encrypted"}, {"php", "application/x-httpd-php"}, {"pic", "image/x-pict"},
      {"pkg", "application/octet-stream"}, {"pki", "application/pkixcmp"}, {"pkipath", "application/pkix-pkipath"}, {"pkpass", "application/vnd.apple.pkpass"},
      {"pl", "application/x-perl"}, {"plb", "application/vnd.3gpp.pic-bw-large"}, {"plc", "application/vnd.mobius.plc"},
      {"plf", "application/vnd.pocketlearn"}, {"pls", "application/pls+xml"}, {"pm", "application/x-perl"}, {"pml", "application/vnd.ctc-posml"},
      {"png", "image/png"}, {"pnm", "image/x-portable-anymap"}, {"portpkg", "application/vnd.macports.portpkg"}, {"pot", "application/vnd.ms-powerpoint"},
      {"potm", "application/vnd.ms-powerpoint.template.macroenabled.12"}, {"potx", "application/vnd.openxmlformats-officedocument.presentationml.template"},
      {"ppam", "application/vnd.ms-powerpoint.addin.macroenabled.12"}, {"ppd", "application/vnd.cups-ppd"}, {"ppm", "image/x-portable-pixmap"},
      {"pps", "application/vnd.ms-powerpoint"}, {"ppsm", "application/vnd.ms-powerpoint.slideshow.macroenabled.12"},
      {"ppsx", "application/vnd.openxmlformats-officedocument.presentationml.slideshow"}, {"ppt", "application/vnd.ms-powerpoint"},
      {"pptm", "application/vnd.ms-powerpoint.presentation.macroenabled.12"},
      {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"}, {"pqa", "application/vnd.palm"}, {"prc", "model/prc"},
      {"pre", "application/vnd.lotus-freelance"}, {"prf", "application/pics-rules"}, {"ps", "application/postscript"},
      {"psb", "application/vnd.3gpp.pic-bw-small"}, {"psd", "image/vnd.adobe.photoshop"}, {"psf", "application/x-font-linux-psf"}, {"pti", "image/prs.pti"},
      {"ptid", "application/vnd.pvi.ptid1"}, {"pub", "application/x-mspublisher"}, {"pvb", "application/vnd.3gpp.pic-bw-var"},
      {"pwn", "application/vnd.3m.post-it-notes"}, {"pya", "audio/vnd.ms-playready.media.pya"}, {"pyv", "video/vnd.ms-playready.media.pyv"},
      {"qam", "application/vnd.epson.quickanime"}, {"qbo", "application/vnd.intu.qbo"}, {"qfx", "application/vnd.intu.qfx"},
      {"qps", "application/vnd.publishare-delta-tree"}, {"qt", "video/quicktime"}, {"qwd", "application/vnd.quark.quarkxpress"},
      {"qwt", "application/vnd.quark.quarkxpress"}, {"qxb", "application/vnd.quark.quarkxpress"}, {"qxd", "application/vnd.quark.quarkxpress"},
      {"qxl", "application/vnd.quark.quarkxpress"}, {"qxt", "application/vnd.quark.quarkxpress"}, {"ra", "audio/x-realaudio"}, {"ram", "audio/x-pn-realaudio"},
      {"raml", "application/raml+yaml"}, {"rapd", "application/route-apd+xml"}, {"rar", "application/x-rar-compressed"}, {"ras", "image/x-cmu-raster"},
      {"rcprofile", "application/vnd.ipunplugged.rcprofile"}, {"rdf", "application/rdf+xml"}, {"rdz", "application/vnd.data-vision.rdz"},
      {"relo", "application/p2p-overlay+xml"}, {"rep", "application/vnd.businessobjects"}, {"res", "application/x-dtbresource+xml"}, {"rgb", "image/x-rgb"},
      {"rif", "application/reginfo+xml"}, {"rip", "audio/vnd.rip"}, {"ris", "application/x-research-info-systems"}, {"rl", "application/resource-lists+xml"},
      {"rlc", "image/vnd.fujixerox.edmics-rlc"}, {"rld", "application/resource-lists-diff+xml"}, {"rm", "application/vnd.rn-realmedia"}, {"rmi", "audio/midi"},
      {"rmp", "audio/x-pn-realaudio-plugin"}, {"rms", "application/vnd.jcp.javame.midlet-rms"}, {"rmvb", "application/vnd.rn-realmedia-vbr"},
      {"rnc", "application/relax-ng-compact-syntax"}, {"rng", "application/xml"}, {"roa", "application/rpki-roa"}, {"roff", "text/troff"},
      {"rp9", "application/vnd.cloanto.rp9"}, {"rpm", "application/x-redhat-package-manager"}, {"rpss", "application/vnd.nokia.radio-presets"},
      {"rpst", "application/vnd.nokia.radio-preset"}, {"rq", "application/sparql-query"}, {"rs", "application/rls-services+xml"},
      {"rsat", "application/atsc-rsat+xml"}, {"rsd", "application/rsd+xml"}, {"rss", "application/rss+xml"}, {"rtf", "text/rtf"}, {"rtx", "text/richtext"},
      {"run", "application/x-makeself"}, {"rusd", "application/route-usd+xml"}, {"s", "text/x-asm"}, {"s3m", "audio/s3m"},
      {"saf", "application/vnd.yamaha.smaf-audio"}, {"sass", "text/x-sass"}, {"sbml", "application/sbml+xml"}, {"sc", "application/vnd.ibm.secure-container"},
      {"scd", "application/x-msschedule"}, {"scm", "application/vnd.lotus-screencam"}, {"scq", "application/scvp-cv-request"},
      {"scs", "application/scvp-cv-response"}, {"scss", "text/x-scss"}, {"scurl", "text/vnd.curl.scurl"}, {"sda", "application/vnd.stardivision.draw"},
      {"sdc", "application/vnd.stardivision.calc"}, {"sdd", "application/vnd.stardivision.impress"}, {"sdkd", "application/vnd.solent.sdkm+xml"},
      {"sdkm", "application/vnd.solent.sdkm+xml"}, {"sdp", "application/sdp"}, {"sdw", "application/vnd.stardivision.writer"}, {"sea", "application/x-sea"},
      {"see", "application/vnd.seemail"}, {"seed", "application/vnd.fdsn.seed"}, {"sema", "application/vnd.sema"}, {"semd", "application/vnd.semd"},
      {"semf", "application/vnd.semf"}, {"ser", "application/java-serialized-object"}, {"sfs", "application/vnd.spotfire.sfs"}, {"sfv", "text/x-sfv"},
      {"sgi", "image/sgi"}, {"sgl", "application/vnd.stardivision.writer-global"}, {"sgm", "text/sgml"}, {"sgml", "text/sgml"}, {"sh", "application/x-sh"},
      {"shar", "application/x-shar"}, {"shex", "text/shex"}, {"shf", "application/shf+xml"}, {"shtml", "text/html;charset=utf-8"},
      {"sid", "image/x-mrsid-image"}, {"sieve", "application/sieve"}, {"sig", "application/pgp-signature"}, {"sil", "audio/silk"}, {"silo", "model/mesh"},
      {"sis", "application/vnd.symbian.install"}, {"sisx", "application/vnd.symbian.install"}, {"sit", "application/x-stuffit"},
      {"sitx", "application/x-stuffitx"}, {"siv", "application/sieve"}, {"skd", "application/vnd.koan"}, {"skm", "application/vnd.koan"},
      {"skp", "application/vnd.koan"}, {"skt", "application/vnd.koan"}, {"sldm", "application/vnd.ms-powerpoint.slide.macroenabled.12"},
      {"sldx", "application/vnd.openxmlformats-officedocument.presentationml.slide"}, {"slim", "text/slim"}, {"slm", "text/slim"},
      {"sls", "application/route-s-tsid+xml"}, {"slt", "application/vnd.epson.salt"}, {"sm", "application/vnd.stepmania.stepchart"},
      {"smf", "application/vnd.stardivision.math"}, {"smi", "application/smil+xml"}, {"smil", "application/smil+xml"}, {"smv", "video/x-smv"},
      {"snd", "audio/basic"}, {"snf", "application/x-font-snf"}, {"so", "application/octet-stream"}, {"spc", "application/x-pkcs7-certificates"},
      {"spdx", "text/spdx"}, {"spf", "application/vnd.yamaha.smaf-phrase"}, {"spl", "application/x-futuresplash"}, {"spot", "text/vnd.in3d.spot"},
      {"spp", "application/scvp-vp-response"}, {"spq", "application/scvp-vp-request"}, {"spx", "audio/ogg"}, {"sql", "application/x-sql"},
      {"src", "application/x-wais-source"}, {"srt", "application/x-subrip"}, {"sru", "application/sru+xml"}, {"srx", "application/sparql-results+xml"},
      {"ssdl", "application/ssdl+xml"}, {"sse", "application/vnd.kodak-descriptor"}, {"ssf", "application/vnd.epson.ssf"}, {"ssml", "application/ssml+xml"},
      {"st", "application/vnd.sailingtracker.track"}, {"stc", "application/vnd.sun.xml.calc.template"}, {"std", "application/vnd.sun.xml.draw.template"},
      {"stf", "application/vnd.wt.stf"}, {"sti", "application/vnd.sun.xml.impress.template"}, {"stk", "application/hyperstudio"}, {"stl", "model/stl"},
      {"stpx", "model/step+xml"}, {"stpz", "model/step+zip"}, {"str", "application/vnd.pg.format"}, {"stw", "application/vnd.sun.xml.writer.template"},
      {"styl", "text/stylus"}, {"stylus", "text/stylus"}, {"sub", "text/vnd.dvb.subtitle"}, {"sus", "application/vnd.sus-calendar"},
      {"susp", "application/vnd.sus-calendar"}, {"sv4cpio", "application/x-sv4cpio"}, {"sv4crc", "application/x-sv4crc"},
      {"svc", "application/vnd.dvb.service"}, {"svd", "application/vnd.svd"}, {"svg", "image/svg+xml"}, {"svgz", "image/svg+xml"},
      {"swa", "application/x-director"}, {"swf", "application/x-shockwave-flash"}, {"swi", "application/vnd.aristanetworks.swi"},
      {"sxc", "application/vnd.sun.xml.calc"}, {"sxd", "application/vnd.sun.xml.draw"}, {"sxg", "application/vnd.sun.xml.writer.global"},
      {"sxi", "application/vnd.sun.xml.impress"}, {"sxm", "application/vnd.sun.xml.math"}, {"sxw", "application/vnd.sun.xml.writer"}, {"t", "text/troff"},
      {"t3", "application/x-t3vm-image"}, {"t38", "image/t38"}, {"tao", "application/vnd.tao.intent-module-archive"}, {"tap", "image/vnd.tencent.tap"},
      {"tar", "application/x-tar"}, {"tcap", "application/vnd.3gpp2.tcap"}, {"tcl", "application/x-tcl"}, {"td", "application/urc-targetdesc+xml"},
      {"teacher", "application/vnd.smart.teacher"}, {"tei", "application/tei+xml"}, {"tex", "application/x-tex"}, {"texi", "application/x-texinfo"},
      {"texinfo", "application/x-texinfo"}, {"text", "text/plain;charset=utf-8"}, {"tfi", "application/thraud+xml"}, {"tfm", "application/x-tex-tfm"},
      {"tfx", "image/tiff-fx"}, {"tga", "image/x-tga"}, {"thmx", "application/vnd.ms-officetheme"}, {"tif", "image/tiff"}, {"tiff", "image/tiff"},
      {"tk", "application/x-tcl"}, {"tmo", "application/vnd.tmobile-livetv"}, {"toml", "application/toml"}, {"torrent", "application/x-bittorrent"},
      {"tpl", "application/vnd.groove-tool-template"}, {"tpt", "application/vnd.trid.tpt"}, {"tr", "text/troff"}, {"tra", "application/vnd.trueapp"},
      {"trig", "application/trig"}, {"trm", "application/x-msterminal"}, {"ts", "text/javascript;charset=utf-8"}, {"tsd", "application/timestamped-data"},
      {"tsv", "text/tab-separated-values"}, {"tsx", "text/javascript;charset=utf-8"}, {"ttc", "font/collection"}, {"ttf", "font/ttf"}, {"ttl", "text/turtle"},
      {"ttml", "application/ttml+xml"}, {"twd", "application/vnd.simtech-mindmapper"}, {"twds", "application/vnd.simtech-mindmapper"},
      {"txd", "application/vnd.genomatix.tuxedo"}, {"txf", "application/vnd.mobius.txf"}, {"txt", "text/plain;charset=utf-8"},
      {"u32", "application/x-authorware-bin"}, {"u3d", "model/u3d"}, {"ubj", "application/ubjson"}, {"udeb", "application/x-debian-package"},
      {"ufd", "application/vnd.ufdl"}, {"ufdl", "application/vnd.ufdl"}, {"ulx", "application/x-glulx"}, {"umj", "application/vnd.umajin"},
      {"uoml", "application/vnd.uoml+xml"}, {"uri", "text/uri-list"}, {"uris", "text/uri-list"}, {"urls", "text/uri-list"}, {"usdz", "model/vnd.usdz+zip"},
      {"ustar", "application/x-ustar"}, {"utz", "application/vnd.uiq.theme"}, {"uu", "text/x-uuencode"}, {"uva", "audio/vnd.dece.audio"},
      {"uvd", "application/vnd.dece.data"}, {"uvf", "application/vnd.dece.data"}, {"uvg", "image/vnd.dece.graphic"}, {"uvh", "video/vnd.dece.hd"},
      {"uvi", "image/vnd.dece.graphic"}, {"uvm", "video/vnd.dece.mobile"}, {"uvp", "video/vnd.dece.pd"}, {"uvs", "video/vnd.dece.sd"},
      {"uvt", "application/vnd.dece.ttml+xml"}, {"uvu", "video/vnd.uvvu.mp4"}, {"uvv", "video/vnd.dece.video"}, {"uvva", "audio/vnd.dece.audio"},
      {"uvvd", "application/vnd.dece.data"}, {"uvvf", "application/vnd.dece.data"}, {"uvvg", "image/vnd.dece.graphic"}, {"uvvh", "video/vnd.dece.hd"},
      {"uvvi", "image/vnd.dece.graphic"}, {"uvvm", "video/vnd.dece.mobile"}, {"uvvp", "video/vnd.dece.pd"}, {"uvvs", "video/vnd.dece.sd"},
      {"uvvt", "application/vnd.dece.ttml+xml"}, {"uvvu", "video/vnd.uvvu.mp4"}, {"uvvv", "video/vnd.dece.video"},
      {"uvvx", "application/vnd.dece.unspecified"}, {"uvvz", "application/vnd.dece.zip"}, {"uvx", "application/vnd.dece.unspecified"},
      {"uvz", "application/vnd.dece.zip"}, {"vbox", "application/x-virtualbox-vbox"}, {"vbox-e", "application/x-virtualbox-vbox-extpack"},
      {"vcard", "text/vcard"}, {"vcd", "application/x-cdlink"}, {"vcf", "text/x-vcard"}, {"vcg", "application/vnd.groove-vcard"}, {"vcs", "text/x-vcalendar"},
      {"vcx", "application/vnd.vcx"}, {"vdi", "application/x-virtualbox-vdi"}, {"vds", "model/vnd.sap.vds"}, {"vhd", "application/x-virtualbox-vhd"},
      {"vis", "application/vnd.visionary"}, {"viv", "video/vnd.vivo"}, {"vmdk", "application/x-virtualbox-vmdk"}, {"vob", "video/x-ms-vob"},
      {"vor", "application/vnd.stardivision.writer"}, {"vox", "application/x-authorware-bin"}, {"vrml", "model/vrml"}, {"vsd", "application/vnd.visio"},
      {"vsf", "application/vnd.vsf"}, {"vss", "application/vnd.visio"}, {"vst", "application/vnd.visio"}, {"vsw", "application/vnd.visio"},
      {"vtf", "image/vnd.valve.source.texture"}, {"vtt", "text/vtt"}, {"vtu", "model/vnd.vtu"}, {"vxml", "application/voicexml+xml"},
      {"w3d", "application/x-director"}, {"wad", "application/x-doom"}, {"wadl", "application/vnd.sun.wadl+xml"}, {"war", "application/java-archive"},
      {"wasm", "application/wasm"}, {"wav", "audio/x-wav"}, {"wax", "audio/x-ms-wax"}, {"wbmp", "image/vnd.wap.wbmp"},
      {"wbs", "application/vnd.criticaltools.wbs+xml"}, {"wbxml", "application/vnd.wap.wbxml"}, {"wcm", "application/vnd.ms-works"},
      {"wdb", "application/vnd.ms-works"}, {"wdp", "image/vnd.ms-photo"}, {"weba", "audio/webm"}, {"webapp", "application/x-web-app-manifest+json"},
      {"webm", "video/webm"}, {"webp", "image/webp"}, {"wg", "application/vnd.pmi.widget"}, {"wgt", "application/widget"},
      {"wif", "application/watcherinfo+xml"}, {"wks", "application/vnd.ms-works"}, {"wm", "video/x-ms-wm"}, {"wma", "audio/x-ms-wma"},
      {"wmd", "application/x-ms-wmd"}, {"wmf", "image/wmf"}, {"wml", "text/vnd.wap.wml"}, {"wmlc", "application/vnd.wap.wmlc"},
      {"wmls", "text/vnd.wap.wmlscript"}, {"wmlsc", "application/vnd.wap.wmlscriptc"}, {"wmv", "video/x-ms-wmv"}, {"wmx", "video/x-ms-wmx"},
      {"wmz", "application/x-msmetafile"}, {"woff", "font/woff"}, {"woff2", "font/woff2"}, {"wpd", "application/vnd.wordperfect"},
      {"wpl", "application/vnd.ms-wpl"}, {"wps", "application/vnd.ms-works"}, {"wqd", "application/vnd.wqd"}, {"wri", "application/x-mswrite"},
      {"wrl", "model/vrml"}, {"wsc", "message/vnd.wfa.wsc"}, {"wsdl", "application/wsdl+xml"}, {"wspolicy", "application/wspolicy+xml"},
      {"wtb", "application/vnd.webturbo"}, {"wvx", "video/x-ms-wvx"}, {"x32", "application/x-authorware-bin"}, {"x3d", "model/x3d+xml"},
      {"x3db", "model/x3d+fastinfoset"}, {"x3dv", "model/x3d+vrml"}, {"x3dz", "model/x3d+xml"}, {"xaml", "application/xaml+xml"},
      {"xap", "application/x-silverlight-app"}, {"xar", "application/vnd.xara"}, {"xav", "application/xcap-att+xml"}, {"xbap", "application/x-ms-xbap"},
      {"xbd", "application/vnd.fujixerox.docuworks.binder"}, {"xbm", "image/x-xbitmap"}, {"xca", "application/xcap-caps+xml"},
      {"xcs", "application/calendar+xml"}, {"xdf", "application/xcap-diff+xml"}, {"xdm", "application/vnd.syncml.dm+xml"},
      {"xdp", "application/vnd.adobe.xdp+xml"}, {"xdw", "application/vnd.fujixerox.docuworks"}, {"xel", "application/xcap-el+xml"},
      {"xenc", "application/xenc+xml"}, {"xer", "application/patch-ops-error+xml"}, {"xfdf", "application/vnd.adobe.xfdf"}, {"xfdl", "application/vnd.xfdl"},
      {"xht", "application/xhtml+xml"}, {"xhtml", "application/xhtml+xml"}, {"xif", "image/vnd.xiff"}, {"xla", "application/vnd.ms-excel"},
      {"xlam", "application/vnd.ms-excel.addin.macroenabled.12"}, {"xlc", "application/vnd.ms-excel"}, {"xlf", "application/xliff+xml"},
      {"xlm", "application/vnd.ms-excel"}, {"xls", "application/vnd.ms-excel"}, {"xlsb", "application/vnd.ms-excel.sheet.binary.macroenabled.12"},
      {"xlsm", "application/vnd.ms-excel.sheet.macroenabled.12"}, {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
      {"xlt", "application/vnd.ms-excel"}, {"xltm", "application/vnd.ms-excel.template.macroenabled.12"},
      {"xltx", "application/vnd.openxmlformats-officedocument.spreadsheetml.template"}, {"xlw", "application/vnd.ms-excel"}, {"xm", "audio/xm"},
      {"xml", "application/xml"}, {"xns", "application/xcap-ns+xml"}, {"xo", "application/vnd.olpc-sugar"}, {"xop", "application/xop+xml"},
      {"xpi", "application/x-xpinstall"}, {"xpl", "application/xproc+xml"}, {"xpm", "image/x-xpixmap"}, {"xpr", "application/vnd.is-xpr"},
      {"xps", "application/vnd.ms-xpsdocument"}, {"xpw", "application/vnd.intercon.formnet"}, {"xpx", "application/vnd.intercon.formnet"},
      {"xsd", "application/xml"}, {"xsl", "application/xslt+xml"}, {"xslt", "application/xslt+xml"}, {"xsm", "application/vnd.syncml+xml"},
      {"xspf", "application/xspf+xml"}, {"xul", "application/vnd.mozilla.xul+xml"}, {"xvm", "application/xv+xml"}, {"xvml", "application/xv+xml"},
      {"xwd", "image/x-xwindowdump"}, {"xyz", "chemical/x-xyz"}, {"xz", "application/x-xz"}, {"yaml", "text/yaml"}, {"yang", "application/yang"},
      {"yin", "application/yin+xml"}, {"yml", "text/yaml"}, {"ymp", "text/x-suse-ymp"}, {"z1", "application/x-zmachine"}, {"z2", "application/x-zmachine"},
      {"z3", "application/x-zmachine"}, {"z4", "application/x-zmachine"}, {"z5", "application/x-zmachine"}, {"z6", "application/x-zmachine"},
      {"z7", "application/x-zmachine"}, {"z8", "application/x-zmachine"}, {"zaz", "application/vnd.zzazz.deck+xml"}, {"zip", "application/zip"},
      {"zir", "application/vnd.zul"}, {"zirz", "application/vnd.zul"}, {"zmm", "application/vnd.handheld-entertainment+xml"},
    };

    // std.fs.path.extension(filename) without the dot, lowercased.
    std::string filenameExtension(std::string_view filename) {
      std::string_view base = filename;
      while (!base.empty() && base.back() == '/') base.remove_suffix(1);
      const std::size_t slash = base.rfind('/');
      if (slash != std::string_view::npos) base.remove_prefix(slash + 1);
      const std::size_t dot = base.rfind('.');
      if (dot == std::string_view::npos || dot == 0) return "";
      return asciiLowercase(base.substr(dot + 1));
    }

    std::string_view typeForExtension(std::string_view extension) {
      if (extension.empty()) return {};
      auto position = std::lower_bound(std::begin(kExtensionTypes), std::end(kExtensionTypes), extension,
                                       [](const ExtensionType& entry, std::string_view target) { return entry.extension < target; });
      if (position == std::end(kExtensionTypes) || position->extension != extension) return {};
      return position->type;
    }

    bool startsWithBytes(std::string_view bytes, std::string_view prefix) { return bytes.size() >= prefix.size() && bytes.substr(0, prefix.size()) == prefix; }

    // Bun's MimeType.sniff as its multipart parser applies it.
    std::string_view sniffedType(std::string_view bytes) {
      using namespace std::string_view_literals;
      if (startsWithBytes(bytes, "\x89PNG\r\n\x1A\n"sv)) return "image/png";
      if (startsWithBytes(bytes, "\xFF\xD8\xFF"sv)) return "image/jpeg";
      if (startsWithBytes(bytes, "GIF89a"sv)) return "image/gif";
      if (startsWithBytes(bytes, "BM"sv)) return "image/bmp";
      if (startsWithBytes(bytes, "II*\0"sv) || startsWithBytes(bytes, "MM\0*"sv)) return "image/tiff";
      return {};
    }

    CodedTypeError formDataError(std::string message) { return CodedTypeError(std::move(message), "ERR_FORMDATA_PARSE_ERROR"); }

    std::string_view trimChars(std::string_view text, std::string_view characters) {
      const std::size_t start = text.find_first_not_of(characters);
      if (start == std::string_view::npos) return {};
      const std::size_t end = text.find_last_not_of(characters);
      return text.substr(start, end - start + 1);
    }

    std::string decodeUtf8KeepingBom(std::string_view bytes) { return decodeUtf8(bytes, TextDecoderOptions{.fatal = false, .ignoreBOM = true}); }

    FormParts parseMultipart(std::string_view input, std::string_view boundary) {
      if (boundary.size() > limits::kMultipartBoundaryBytes) throw formDataError("FormData parse error boundary is too long");
      const std::string finalBoundary = "--" + std::string(boundary) + "--";
      const std::size_t finalIndex = input.rfind(finalBoundary);
      if (finalIndex == std::string_view::npos) throw formDataError("FormData parse error missing final boundary");
      const std::string separator = "--" + std::string(boundary) + "\r\n";
      input = input.substr(0, finalIndex);
      FormParts parts;
      std::size_t partCount = 0;
      for (auto next = input.find(separator); next != std::string_view::npos;) {
        if (++partCount > limits::kMultipartParts) throw formDataError("FormData part count exceeds limit");
        input.remove_prefix(next + separator.size());
        next = input.find(separator);
        const auto part = input.substr(0, next);
        const auto head = part.substr(0, limits::kHttpHeaderBlockBytes);
        const std::size_t headerEnd = head.find("\r\n\r\n");
        if (headerEnd == std::string_view::npos) throw formDataError("FormData parse error is missing header end");
        auto body = part.substr(headerEnd + 4);
        if (body.ends_with("\r\n")) body.remove_suffix(2);
        if (body.size() > limits::kMultipartPartBytes) throw formDataError("FormData part size exceeds limit");
        std::string_view name, filename;
        bool isFile = false;
        std::size_t fieldCount = 0;
        for (auto header = part.substr(0, headerEnd + 2); !header.empty();) {
          const auto end = header.find("\r\n");
          const auto line = header.substr(0, end);
          header.remove_prefix(end + 2);
          if (line.empty()) continue;
          if (++fieldCount > limits::kHttpHeaderFields) throw formDataError("FormData header count exceeds limit");
          const std::size_t colon = line.find(':');
          if (colon == std::string_view::npos) throw formDataError("FormData parse error is missing header colon separator");
          const std::string_view key = line.substr(0, colon);
          std::string_view value = line.substr(colon + 1);
          if (!equalsIgnoringAsciiCase(key, "content-disposition")) continue;
          value = trimChars(value, " ");
          if (startsWithBytes(value, "form-data;")) value = trimChars(value.substr(10), " ");
          for (std::size_t equals = value.find('='); equals != std::string_view::npos; equals = value.find('=')) {
            const std::string_view parameter = trimChars(value.substr(0, equals), " ;");
            value = value.substr(equals + 1);
            if (startsWithBytes(value, "\"")) value.remove_prefix(1);
            std::string_view fieldValue = value;
            std::size_t index = 0;
            for (; index < fieldValue.size(); ++index) {
              if (fieldValue[index] == '"') {
                fieldValue = fieldValue.substr(0, index);
                break;
              }
              if (fieldValue[index] == '\\' && index + 1 < fieldValue.size() && fieldValue[index + 1] == '"') ++index;
            }
            value = value.substr(std::min(index + 1, value.size()));
            if (equalsIgnoringAsciiCase(parameter, "name")) {
              name = fieldValue;
            } else if (equalsIgnoringAsciiCase(parameter, "filename")) {
              filename = fieldValue;
              isFile = true;
            }
            if (!name.empty() && !filename.empty()) break;
            const std::size_t semicolon = value.find(';');
            if (semicolon == std::string_view::npos) break;
            value = value.substr(semicolon + 1);
          }
        }
        if (name.empty()) continue;
        if (isFile) {
          std::string_view type = typeForExtension(filenameExtension(filename));
          if (type.empty()) type = sniffedType(body);
          std::optional<std::string> fileName;
          if (!filename.empty() && !body.empty()) fileName = decodeUtf8KeepingBom(filename);
          parts.emplace_back(decodeUtf8KeepingBom(name), FormFile{std::move(fileName), std::string(type), Bytes(body.begin(), body.end())});
        } else {
          parts.emplace_back(decodeUtf8KeepingBom(name), decodeUtf8(body));
        }
      }
      return parts;
    }

    // The boundary parameter of a multipart content type.
    std::optional<std::string> multipartBoundary(std::string_view contentType) {
      std::size_t semicolon = contentType.find(';');
      while (semicolon != std::string_view::npos) {
        std::string_view parameter = contentType.substr(semicolon + 1);
        const std::size_t next = parameter.find(';');
        semicolon = next == std::string_view::npos ? std::string_view::npos : semicolon + 1 + next;
        parameter = parameter.substr(0, next);
        while (!parameter.empty() && (parameter.front() == ' ' || parameter.front() == '\t')) parameter.remove_prefix(1);
        if (parameter.size() < 9 || !equalsIgnoringAsciiCase(parameter.substr(0, 9), "boundary=")) continue;
        std::string_view value = parameter.substr(9);
        if (value.empty()) return std::nullopt;
        if (value.front() == '"') {
          if (value.size() < 2 || value.back() != '"') return std::nullopt;
          return std::string(value.substr(1, value.size() - 2));
        }
        return std::string(value);
      }
      return std::nullopt;
    }

    // ---- bodies -------------------------------------------------------------

    CodedTypeError bodyUsedError() { return CodedTypeError("Body already used", "ERR_BODY_ALREADY_USED"); }

    bool streamDisturbed(const ByteStream& stream) { return stream.state()->disturbed; }

    void extractBody(BodyState& body, BodyInit init) {
      body = BodyState();
      std::visit(Overloaded{
                     [&](std::nullptr_t) {},
                     [&](std::string& text) {
                       body.kind = BodyState::Kind::buffer;
                       body.text = std::move(text);
                     },
                     [&](Bytes& bytes) {
                       body.kind = BodyState::Kind::buffer;
                       body.bytes = std::make_shared<const Bytes>(std::move(bytes));
                     },
                     [&](ByteStream& stream) {
                       if (stream.locked() || streamDisturbed(stream)) throw TypeError("Body object should not be disturbed or locked");
                       body.kind = BodyState::Kind::stream;
                       body.stream = std::move(stream);
                     },
                     [&](URLSearchParams& params) {
                       body.kind = BodyState::Kind::buffer;
                       body.bytes = bytesOfText(params.toString());
                       body.contentType = "application/x-www-form-urlencoded;charset=UTF-8";
                     }},
                 init);
    }

    const std::shared_ptr<const Bytes>& bufferBytes(BodyState& body) {
      if (!body.bytes) body.bytes = body.text ? bytesOfText(*body.text) : emptyBytes();
      return body.bytes;
    }

    bool isBodyUsed(const BodyState& body) {
      switch (body.kind) {
        case BodyState::Kind::none: return false;
        case BodyState::Kind::stream: return streamDisturbed(*body.stream);
        case BodyState::Kind::buffer: return body.used || (body.stream && (body.stream->locked() || streamDisturbed(*body.stream)));
      }
      return false;
    }

    std::optional<ByteStream> bodyStream(BodyState& body) {
      if (body.kind == BodyState::Kind::none) return std::nullopt;
      if (body.stream) return body.stream;
      if (body.used) {
        UnderlyingSource<ByteChunk> source;
        source.start = [](ReadableStreamDefaultController<ByteChunk> controller) { controller.close(); };
        ByteStream stream(std::move(source));
        (void)stream.getReader();
        body.stream = stream;
      } else {
        body.stream = bufferStream(bufferBytes(body));
      }
      return body.stream;
    }

    // What a body read produced: the original JS string of a string body, or bytes.
    struct Consumed {
      std::optional<std::string> text;
      std::shared_ptr<const Bytes> bytes;
      bool streamed = false;
    };

    // The synchronous part of a body read, run when the JS method is called:
    // validates, marks the body used and locks a stream body.
    struct ConsumeStart {
      Reason error;
      Consumed buffered;
      std::optional<ReadableStreamDefaultReader<ByteChunk>> reader;
    };

    ConsumeStart startConsume(BodyState& body) {
      ConsumeStart start;
      if (isBodyUsed(body)) {
        start.error = makeReason(bodyUsedError());
        return start;
      }
      switch (body.kind) {
        case BodyState::Kind::none:
          start.buffered.bytes = emptyBytes();
          break;
        case BodyState::Kind::buffer:
          body.used = true;
          if (body.text) start.buffered.text = body.text;
          else start.buffered.bytes = bufferBytes(body);
          break;
        case BodyState::Kind::stream:
          if (body.stream->locked()) {
            start.error = makeReason(CodedTypeError("Invalid state: ReadableStream is locked", "ERR_INVALID_STATE"));
            break;
          }
          start.reader = body.stream->getReader();
          break;
      }
      return start;
    }

    Task<Consumed> finishConsume(ConsumeStart start) {
      if (start.error) rethrow(start.error);
      if (!start.reader) {
        co_await microtask();
        co_return start.buffered;
      }
      auto reader = std::move(*start.reader);
      auto release = scopeExit([&reader] { reader.releaseLock(); });
      auto collected = std::make_shared<Bytes>();
      for (;;) {
        Task<ReadableStreamReadResult<ByteChunk>> reading = reader.read();
        const ReadableStreamReadResult<ByteChunk> step = co_await std::move(reading);
        if (step.done) break;
        collected->insert(collected->end(), step.value->begin(), step.value->end());
      }
      co_return Consumed{std::nullopt, std::move(collected), true};
    }

    std::string consumedText(const Consumed& consumed) {
      if (consumed.text) return *consumed.text;
      return decodeUtf8(std::span<const std::uint8_t>(*consumed.bytes));
    }

    Bytes consumedBytes(const Consumed& consumed) {
      if (consumed.text) return encodeUtf8(*consumed.text);
      return *consumed.bytes;
    }

    // GCC 12 miscompiles temporaries inside a co_await full-expression, so every
    // awaited Task here is a named local first.
    Task<std::string> readText(ConsumeStart start) {
      Task<Consumed> consuming = finishConsume(std::move(start));
      const Consumed consumed = co_await std::move(consuming);
      co_return consumedText(consumed);
    }

    Task<Bytes> readBytes(ConsumeStart start) {
      Task<Consumed> consuming = finishConsume(std::move(start));
      const Consumed consumed = co_await std::move(consuming);
      co_return consumedBytes(consumed);
    }

    // A stream body is parsed with JSON.parse (JavaScriptCore's messages); a
    // buffered one with Bun's own messages, chosen by whether it had any bytes.
    Task<json::value> readJson(ConsumeStart start) {
      Task<Consumed> consuming = finishConsume(std::move(start));
      const Consumed consumed = co_await std::move(consuming);
      if (consumed.streamed) co_return jsonParse(consumedText(consumed));
      const bool emptyBody = consumed.text ? consumed.text->empty() : consumed.bytes->empty();
      co_return jsonParseBody(consumedText(consumed), emptyBody);
    }

    // How a body is cloned: by clone() (already checked usable), by `new
    // Request(input)` / a Request used as init, or by `new Request(input, init)`.
    enum class CloneMode : std::uint8_t { clone, input, inputWithInit };

    // Body.Value.clone(): both sides end up with the same bytes (a string body
    // becomes bytes), or with one branch each of a teed stream. A body whose
    // stream is already disturbed (or a buffer whose `.body` stream is) is
    // shared as is, so the copy reads as used too.
    BodyState cloneBody(BodyState& source, CloneMode mode) {
      if (source.kind == BodyState::Kind::none) return BodyState();
      if (source.stream && (streamDisturbed(*source.stream) || (source.kind == BodyState::Kind::buffer && source.stream->locked()))) return source;
      BodyState copy;
      copy.kind = source.kind;
      copy.contentType = source.contentType;
      if (source.kind == BodyState::Kind::buffer) {
        if (source.used) {
          if (mode == CloneMode::inputWithInit) throw TypeError("Body object should not be disturbed or locked");
          copy.bytes = emptyBytes();
          return copy;
        }
        copy.bytes = bufferBytes(source);
        source.text.reset();
        source.stream.reset();
        return copy;
      }
      if (source.stream->locked()) throw CodedTypeError("Invalid state: ReadableStream is locked", "ERR_INVALID_STATE");
      auto [first, second] = source.stream->tee();
      source.stream = std::move(first);
      copy.stream = std::move(second);
      return copy;
    }

    void addContentType(Headers& headers, const BodyState& body) {
      if (!body.contentType.empty() && !headers.has("content-type")) headers.set("content-type", body.contentType);
    }

    template <class State> Headers& materializedHeaders(State& state) {
      if (state.lazyContentType) {
        state.lazyContentType = false;
        if (state.body.kind == BodyState::Kind::buffer && !state.body.used && !state.body.stream) addContentType(state.headers, state.body);
      }
      return state.headers;
    }

    // ---- Response / Request fields --------------------------------------------

    // A JS number status as Bun converts it: truncated toward zero, NaN as 0,
    // saturated to the int64 range.
    std::int64_t statusInteger(double value) {
      if (std::isnan(value)) return 0;
      if (value >= 9223372036854775807.0) return std::numeric_limits<std::int64_t>::max();
      if (value <= -9223372036854775808.0) return std::numeric_limits<std::int64_t>::min();
      return static_cast<std::int64_t>(value);
    }

    int validatedStatus(double value) {
      const std::int64_t status = statusInteger(value);
      if (status != 101 && (status < 200 || status > 599))
        throw RangeError("The status provided (" + std::to_string(status) + ") must be 101 or in the range of [200, 599]");
      return static_cast<int>(status);
    }

    constexpr std::array<std::string_view, 35> kMethods = {
        "ACL",      "BIND",       "CHECKOUT", "CONNECT",  "COPY",      "DELETE", "GET",       "HEAD",   "LINK",   "LOCK",   "M-SEARCH",    "MERGE",
        "MKACTIVITY", "MKCALENDAR", "MKCOL",  "MOVE",     "NOTIFY",    "OPTIONS", "PATCH",    "POST",   "PROPFIND", "PROPPATCH", "PURGE", "PUT",
        "QUERY",    "REBIND",     "REPORT",   "SEARCH",   "SOURCE",    "SUBSCRIBE", "TRACE",  "UNBIND", "UNLINK", "UNLOCK", "UNSUBSCRIBE"};

    std::string normalizedMethod(std::string_view method) {
      for (std::string_view known : kMethods)
        if (method == known || method == asciiLowercase(known)) return std::string(known);
      return "GET";
    }

    std::string validatedRedirect(std::string_view redirect) {
      if (redirect == "follow" || redirect == "manual" || redirect == "error") return std::string(redirect);
      throw CodedTypeError("redirect must be one of 'follow', 'manual' or 'error'", "ERR_INVALID_ARG_TYPE");
    }

    std::string validatedCache(std::string_view cache) {
      if (cache == "default" || cache == "no-store" || cache == "reload" || cache == "no-cache" || cache == "force-cache" || cache == "only-if-cached")
        return std::string(cache);
      throw CodedTypeError("cache must be one of 'default', 'no-store', 'reload', 'no-cache', 'force-cache' or 'only-if-cached'", "ERR_INVALID_ARG_TYPE");
    }

    std::string requestHref(std::string_view url) {
      if (url.empty()) throw JsError("Failed to construct 'Request': url is required.");
      auto parsed = URL::parse(url);
      if (!parsed) throw CodedTypeError("Failed to construct 'Request': Invalid URL \"" + std::string(url) + "\"", "ERR_INVALID_URL");
      return parsed->href();
    }
  }

  const FormValue* findFormPart(const FormParts& parts, std::string_view name) {
    const auto found = std::ranges::find(parts, name, &FormParts::value_type::first);
    return found == parts.end() ? nullptr : &found->second;
  }

  InboundFormDecoder::InboundFormDecoder(std::string_view contentType) {
    if (contentType.size() > limits::kHttpHeaderBlockBytes) throw formDataError("FormData content type exceeds limit");
    if (containsIgnoringAsciiCase(contentType, "application/x-www-form-urlencoded")) return;
    if (containsIgnoringAsciiCase(contentType, "multipart/form-data")) boundary_ = multipartBoundary(contentType);
    if (!boundary_) throw formDataError("Can't decode form data from body because of incorrect MIME type/boundary");
  }

  FormParts InboundFormDecoder::decode(std::span<const std::uint8_t> body) const {
    const auto limit = boundary_ ? limits::kImageEditMultipartBytes : limits::kUrlQueryBytes;
    if (body.size() > limit) throw formDataError("FormData body exceeds limit");
    const auto bytes = body.empty() ? std::string_view() : std::string_view(reinterpret_cast<const char*>(body.data()), body.size());
    if (boundary_) return parseMultipart(bytes, *boundary_);
    const URLSearchParams params{decodeUtf8(bytes)};
    FormParts parts;
    parts.reserve(params.size());
    for (auto [name, value] : params) parts.emplace_back(std::move(name), std::move(value));
    return parts;
  }

  // ---- Response -------------------------------------------------------------------

  Response::Response() : state_(std::make_shared<detail::ResponseState>()) {}

  Response::Response(BodyInit body, ResponseInit init) : state_(std::make_shared<detail::ResponseState>()) {
    if (init.status) state_->status = validatedStatus(*init.status);
    if (init.statusText) state_->statusText = std::move(*init.statusText);
    extractBody(state_->body, std::move(body));
    if (init.headers) {
      state_->headers = std::move(*init.headers);
      addContentType(state_->headers, state_->body);
    } else {
      state_->lazyContentType = !state_->body.contentType.empty();
    }
  }

  Response Response::json(const json::value& data, ResponseInit init) {
    Response response(BodyInit(jsonStringify(data)), std::move(init));
    if (!response.state_->headers.has("content-type")) response.state_->headers.set("content-type", "application/json;charset=utf-8");
    return response;
  }

  Response Response::redirect(std::string_view url, double status) {
    const std::int64_t code = statusInteger(status);
    if (code != 301 && code != 302 && code != 303 && code != 307 && code != 308) throw RangeError("Failed to execute 'redirect' on 'Response': Invalid status code");
    Response response(BodyInit(Bytes{}));
    response.state_->status = static_cast<int>(code);
    if (!url.empty()) {
      auto parsed = URL::parse(url);
      response.state_->headers.append("location", parsed ? parsed->href() : std::string(url));
    }
    return response;
  }

  Response Response::error() {
    Response response(BodyInit(Bytes{}));
    response.state_->status = 0;
    response.state_->errorType = true;
    return response;
  }

  int Response::status() const { return state_->status; }
  const std::string& Response::statusText() const { return state_->statusText; }
  bool Response::ok() const { return state_->status >= 200 && state_->status <= 299; }
  Headers& Response::headers() const { return materializedHeaders(*state_); }
  const std::string& Response::url() const { return state_->url; }
  bool Response::redirected() const { return state_->redirected; }
  std::string Response::type() const { return state_->errorType || state_->status == 101 ? "error" : "default"; }

  std::optional<ByteStream> Response::body() const { return bodyStream(state_->body); }
  bool Response::bodyUsed() const { return isBodyUsed(state_->body); }
  Task<std::string> Response::text() const { return readText(startConsume(state_->body)); }
  Task<json::value> Response::json() const { return readJson(startConsume(state_->body)); }
  Task<Bytes> Response::arrayBuffer() const { return readBytes(startConsume(state_->body)); }
  Task<Bytes> Response::bytes() const { return readBytes(startConsume(state_->body)); }

  Response Response::clone() const {
    const BodyState& body = state_->body;
    if (isBodyUsed(body) || (body.stream && body.stream->locked())) throw CodedTypeError("Body is disturbed or locked", "ERR_BODY_ALREADY_USED");
    auto copy = std::make_shared<detail::ResponseState>();
    copy->status = state_->status;
    copy->statusText = state_->statusText;
    copy->headers = headers();
    copy->url = state_->url;
    copy->redirected = state_->redirected;
    copy->errorType = state_->errorType;
    copy->body = cloneBody(state_->body, CloneMode::clone);
    return Response(Adopt{}, std::move(copy));
  }

  void Response::setFetchResult(std::string url, bool redirected) const {
    state_->url = std::move(url);
    state_->redirected = redirected;
  }

  void Response::setBodyStream(std::optional<ByteStream> stream) const {
    BodyState body;
    body.contentType = state_->body.contentType;
    if (stream) {
      body.kind = BodyState::Kind::stream;
      body.stream = std::move(stream);
    }
    state_->body = std::move(body);
  }

  // ---- Request --------------------------------------------------------------------

  Request::Request(std::string_view url, RequestInit init) : state_(std::make_shared<detail::RequestState>(AbortController().signal())) {
    initialize(requestHref(url), nullptr, std::move(init), true);
  }

  Request::Request(const URL& url, RequestInit init) : state_(std::make_shared<detail::RequestState>(AbortController().signal())) {
    initialize(url.href(), nullptr, std::move(init), true);
  }

  Request::Request(std::string_view url, const Request& init) : Request(URL(requestHref(url)), init) {}

  Request::Request(const URL& url, const Request& init) : state_(std::make_shared<detail::RequestState>(init.signal())) {
    detail::RequestState& state = *state_;
    state.url = url.href();
    state.method = init.method();
    state.headers = init.headers();
    state.redirect = init.redirect();
    state.cache = init.cache();
    state.body = cloneBody(init.state_->body, CloneMode::input);
    addContentType(state.headers, state.body);
  }

  Request::Request(const Request& input, std::optional<RequestInit> init) : state_(std::make_shared<detail::RequestState>(input.signal())) {
    const bool initGiven = init.has_value();
    initialize(input.url(), &input, init ? std::move(*init) : RequestInit(), initGiven);
  }

  void Request::initialize(std::string href, const Request* input, RequestInit init, bool initGiven) {
    detail::RequestState& state = *state_;
    state.url = std::move(href);
    if (input) {
      state.method = input->method();
      state.headers = input->headers();
      state.redirect = input->redirect();
      state.cache = input->cache();
    }
    if (init.method) state.method = normalizedMethod(*init.method);
    const bool eagerHeaders = init.headers.has_value();
    if (init.headers) state.headers = std::move(*init.headers);
    if (init.body) extractBody(state.body, std::move(*init.body));
    else if (input) state.body = cloneBody(input->state_->body, initGiven ? CloneMode::inputWithInit : CloneMode::input);
    if (init.signal) state.signal = std::move(*init.signal);
    if (init.redirect) state.redirect = validatedRedirect(*init.redirect);
    if (init.cache) state.cache = validatedCache(*init.cache);
    if (eagerHeaders) addContentType(state.headers, state.body);
    else state.lazyContentType = !state.body.contentType.empty();
  }

  const std::string& Request::method() const { return state_->method; }
  const std::string& Request::url() const { return state_->url; }
  Headers& Request::headers() const { return materializedHeaders(*state_); }
  const AbortSignal& Request::signal() const { return state_->signal; }
  const std::string& Request::redirect() const { return state_->redirect; }
  const std::string& Request::cache() const { return state_->cache; }

  std::optional<ByteStream> Request::body() const { return bodyStream(state_->body); }
  bool Request::bodyUsed() const { return isBodyUsed(state_->body); }
  Task<std::string> Request::text() const { return readText(startConsume(state_->body)); }
  Task<json::value> Request::json() const { return readJson(startConsume(state_->body)); }
  Task<Bytes> Request::arrayBuffer() const { return readBytes(startConsume(state_->body)); }
  Task<Bytes> Request::bytes() const { return readBytes(startConsume(state_->body)); }

  void detail::transferBody(const Request& input, const Request& outgoing) {
    BodyState& source = input.state()->body;
    BodyState& target = outgoing.state()->body;
    target = source;
    if (source.kind == BodyState::Kind::buffer) source.used = true;
  }

  Request Request::clone() const {
    const BodyState& body = state_->body;
    if (isBodyUsed(body) || (body.stream && body.stream->locked())) throw CodedTypeError("Body is disturbed or locked", "ERR_BODY_ALREADY_USED");
    auto copy = std::make_shared<detail::RequestState>(state_->signal);
    copy->method = state_->method;
    copy->url = state_->url;
    copy->headers = headers();
    copy->redirect = state_->redirect;
    copy->cache = state_->cache;
    copy->body = cloneBody(state_->body, CloneMode::clone);
    return Request(Adopt{}, std::move(copy));
  }
}
