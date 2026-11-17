#include "uos/kv_conversion/convert.hpp"

#include "uos/kv_conversion/typed_value.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/sqlite.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/url.hpp"
#include "uos/storage/kv.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uos::kv_conversion::convert {
  namespace {
    using native_schema::NativeReader;
    using native_schema::NativeReadStatement;
    using native_schema::NativeRevision;
    using native_schema::NativeRow;
    using native_schema::NativeSnapshotReader;
    using rt::record::Reader;
    using rt::record::Writer;
    using typed_value::TypedValue;
    namespace bun_sqlite = native_schema::bun_sqlite;
    using LegacyCodec = legacy_codec::LegacyCodec;

    // A bun:sqlite Database where the TS passes it as a NativeReader (its statements as they are).
    NativeReader nativeReader(const rt::sqlite::Database& database) {
      NativeReader reader;
      reader.query = [database](const std::string& sql) {
        const rt::sqlite::Statement statement = database.query(sql);
        NativeReadStatement read;
        read.get = [statement] { return statement.get(); };
        read.all = [statement] { return statement.all(); };
        read.iterate = [statement]() -> bun_sqlite::RowIterator {
          auto rows = std::make_shared<rt::sqlite::RowIterator>(statement.iterate());
          return [rows]() { return rows->next(); };
        };
        return read;
      };
      return reader;
    }

    // ---- column values as JavaScript sees them ------------------------------------------------

    const bun_sqlite::Value* column(const NativeRow& row, std::string_view name) { return row.get(name); }

    // String(value)
    std::string jsString(const bun_sqlite::Value* value) {
      if (value == nullptr) return "undefined";
      if (std::holds_alternative<std::nullptr_t>(*value)) return "null";
      if (const std::int64_t* integer = std::get_if<std::int64_t>(value)) return std::to_string(*integer);
      if (const double* number = std::get_if<double>(value)) return rt::numberToString(*number);
      if (const std::string* text = std::get_if<std::string>(value)) return *text;
      const std::vector<std::uint8_t>& bytes = std::get<std::vector<std::uint8_t>>(*value);
      std::string joined;
      for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index > 0) joined += ",";
        joined += std::to_string(bytes[index]);
      }
      return joined;
    }

    // Number(value)
    double jsNumber(const bun_sqlite::Value* value) {
      if (value == nullptr) return rt::Number::NaN;
      if (std::holds_alternative<std::nullptr_t>(*value)) return 0;
      if (const std::int64_t* integer = std::get_if<std::int64_t>(value)) return static_cast<double>(*integer);
      if (const double* number = std::get_if<double>(value)) return *number;
      return rt::stringToNumber(jsString(value));
    }

    // Buffer.from(value): the bytes of a Uint8Array, a string's UTF-8, Node's TypeError otherwise.
    std::vector<std::uint8_t> bufferFrom(const bun_sqlite::Value* value) {
      if (value != nullptr) {
        if (const std::vector<std::uint8_t>* bytes = std::get_if<std::vector<std::uint8_t>>(value)) return *bytes;
        if (const std::string* text = std::get_if<std::string>(value)) return rt::encodeUtf8(*text);
      }
      std::string received;
      if (value == nullptr) received = "undefined";
      else if (std::holds_alternative<std::nullptr_t>(*value)) received = "null";
      else if (const std::int64_t* integer = std::get_if<std::int64_t>(value)) received = "type bigint (" + std::to_string(*integer) + "n)";
      else received = "type number (" + rt::numberToString(std::get<double>(*value)) + ")";
      throw rt::CodedTypeError("The first argument must be of type string or an instance of Buffer, ArrayBuffer, or Array or an Array-like Object. Received " + received,
                               "ERR_INVALID_ARG_TYPE");
    }

    // ---- files --------------------------------------------------------------------------------

    void syncFile(const std::string& path) {
      const int fd = rt::fs::openSync(path, "r");
      rt::Reason failure;
      try {
        rt::fs::fsyncSync(fd);
      } catch (...) {
        failure = std::current_exception();
      }
      rt::fs::closeSync(fd);
      if (failure) std::rethrow_exception(failure);
    }

    void removeTree(const std::string& path) {
      rt::fs::RmOptions options;
      options.recursive = true;
      options.force = true;
      rt::fs::rmSync(path, options);
    }

    rt::Task<SnapshotIdentity> snapshotIdentity(std::string path) {
      const rt::fs::Stats stat = rt::fs::lstatSync(path);
      if (!stat.isFile()) throw rt::JsError("Source must be a regular SQLite file");
      const std::string wal = path + "-wal";
      if (rt::fs::existsSync(wal) && rt::fs::lstatSync(wal).size != 0) throw rt::JsError("Source must be stopped, drained and checkpointed; nonempty WAL is refused");
      if (rt::fs::existsSync(path + "-journal")) throw rt::JsError("Source has an unsettled rollback journal");
      rt::Task<std::string> digesting = legacy_codec::fileDigest(path);
      std::string sha256 = co_await std::move(digesting);
      SnapshotIdentity identity{
        .sha256 = std::move(sha256),
        .size = std::to_string(stat.size),
        .device = std::to_string(stat.dev),
        .inode = std::to_string(stat.ino),
        .mtime_ns = std::to_string(stat.mtimeNs),
        .wal = rt::fs::existsSync(wal) ? "empty" : "absent",
        .drain_boundary = "checkpointed-no-wal",
      };
      co_return identity;
    }

    rt::Task<SnapshotIdentity> privateSnapshotIdentity(std::string path, std::optional<SnapshotIdentity> expected) {
      static constexpr std::array<std::string_view, 3> sidecars{"-wal", "-shm", "-journal"};
      for (const std::string_view suffix : sidecars) {
        if (rt::fs::existsSync(path + std::string(suffix))) throw rt::JsError("Exclusive conversion snapshot must not have SQLite sidecars");
      }
      rt::Task<SnapshotIdentity> identifying = snapshotIdentity(path);
      SnapshotIdentity identity = co_await std::move(identifying);
      if (expected && !(identity == *expected)) throw rt::JsError("Private conversion snapshot changed");
      co_return identity;
    }

    struct OpenedSnapshot {
      NativeSnapshotReader reader;
      NativeRevision revision;
    };

    rt::Task<OpenedSnapshot> openPrivateSnapshot(std::string path, std::string codec, SnapshotIdentity expected) {
      // URI is explicit on every platform; no normal WAL open may create snapshot sidecars.
      rt::Task<SnapshotIdentity> checking = privateSnapshotIdentity(path, std::move(expected));
      co_await std::move(checking);
      rt::URL uri = rt::pathToFileURL(path);
      uri.searchParams().set("immutable", "1");
      const rt::sqlite::Database database(uri.href(), rt::sqlite::constants::SQLITE_OPEN_READONLY | rt::sqlite::constants::SQLITE_OPEN_URI);
      NativeSnapshotReader reader = native_schema::createNativeSnapshotReader(bun_sqlite::adapt(database));
      std::optional<NativeRevision> revision;
      try {
        revision = native_schema::verifyNativeSchema(reader, codec);
      } catch (...) {
        reader.close();
        throw;
      }
      OpenedSnapshot opened{.reader = std::move(reader), .revision = *revision};
      co_return opened;
    }

    const char* const KV_ROWS = "SELECT k,v,v_encoding,version,seq,expiration_ms FROM kv ORDER BY k";

    std::string digestRows(const NativeReader& db, bool expiryOnly = false) {
      rt::crypto::Hash hash = rt::crypto::createHash("sha256");
      const NativeReadStatement statement = db.query(KV_ROWS);
      const bun_sqlite::RowIterator next = statement.iterate();
      for (;;) {
        const std::optional<NativeRow> row = next();
        if (!row) break;
        json::array fields;
        if (expiryOnly) {
          fields.emplace_back(rt::toHex(bufferFrom(column(*row, "k"))));
          fields.emplace_back(jsString(column(*row, "expiration_ms")));
        } else {
          fields.emplace_back(rt::toHex(bufferFrom(column(*row, "k"))));
          fields.emplace_back(rt::toBase64(bufferFrom(column(*row, "v"))));
          fields.emplace_back(jsString(column(*row, "v_encoding")));
          fields.emplace_back(jsString(column(*row, "version")));
          fields.emplace_back(jsString(column(*row, "seq")));
          fields.emplace_back(jsString(column(*row, "expiration_ms")));
        }
        hash.update(rt::jsonStringify(json::value(std::move(fields))));
        hash.update("\n");
      }
      return hash.digest("hex");
    }

    struct Transcoded {
      std::vector<std::uint8_t> bytes;
      bool changed;
    };

    rt::Task<Transcoded> transcodeValue(NativeRow row, LegacyCodec codec, cursors::ConversionDirection direction) {
      const std::vector<std::uint8_t> value = std::get<std::vector<std::uint8_t>>(*column(row, "v"));
      if (jsNumber(column(row, "v_encoding")) != 1) {
        Transcoded unchanged{.bytes = value, .changed = false};
        co_return unchanged;
      }
      TypedValue graph;
      if (direction == "deno-to-bun") {
        rt::Task<TypedValue> decoding = codec.decode(value);
        graph = co_await std::move(decoding);
      } else {
        graph = typed_value::encodeTypedValue(storage::kv::deserialize()(value));
      }
      const std::vector<std::uint8_t>& key = std::get<std::vector<std::uint8_t>>(*column(row, "k"));
      const cursors::TranscodePersistedCursorResult translated = cursors::transcodePersistedCursor(key, typed_value::decodeTypedValue(graph), direction);
      const TypedValue targetGraph = translated.changed ? typed_value::encodeTypedValue(translated.value) : graph;
      if (direction == "bun-to-deno") {
        rt::Task<std::vector<std::uint8_t>> encoding = codec.encode(targetGraph);
        std::vector<std::uint8_t> bytes = co_await std::move(encoding);
        Transcoded encoded{.bytes = std::move(bytes), .changed = translated.changed};
        co_return encoded;
      }
      const rt::KvValue convertedValue = typed_value::decodeTypedValue(targetGraph);
      storage::kv::assertPortableKvValue(convertedValue);
      std::vector<std::uint8_t> bytes = rt::kvJscSerialize(convertedValue);
      if (rt::jsonStringify(typed_value::encodeTypedValue(storage::kv::deserialize()(bytes))) != rt::jsonStringify(targetGraph))
        throw rt::JsError("Bun typed-value roundtrip changed semantics");
      Transcoded serialized{.bytes = std::move(bytes), .changed = translated.changed};
      co_return serialized;
    }

    struct Transferred {
      double count;
      double cursors;
    };

    rt::Task<Transferred> transferRows(NativeReader input, rt::sqlite::Database output, LegacyCodec codec, ConvertOptions options, NativeRevision revision) {
      double count = 0;
      double cursorsChanged = 0;
      const rt::sqlite::Statement insert = output.query("INSERT INTO kv(k,v,v_encoding,version,seq,expiration_ms) VALUES(?,?,?,?,?,?)");
      const NativeReadStatement rows = input.query(KV_ROWS);
      const bun_sqlite::RowIterator next = rows.iterate();
      for (;;) {
        std::optional<NativeRow> row = next();
        if (!row) break;
        if (options.signal) options.signal->throwIfAborted();
        native_schema::validateNativeRow(*row, revision);
        storage::kv::assertPortableKvKeyBytes(std::get<std::vector<std::uint8_t>>(*column(*row, "k")));
        rt::Task<Transcoded> transcoding = transcodeValue(*row, codec, options.direction);
        const Transcoded converted = co_await std::move(transcoding);
        cursorsChanged += converted.changed ? 1 : 0;
        std::vector<rt::sqlite::Binding> parameters;
        parameters.emplace_back(*column(*row, "k"));
        parameters.emplace_back(converted.bytes);
        parameters.emplace_back(*column(*row, "v_encoding"));
        parameters.emplace_back(*column(*row, "version"));
        parameters.emplace_back(*column(*row, "seq"));
        parameters.emplace_back(*column(*row, "expiration_ms"));
        insert.run(parameters);
        count++;
      }
      Transferred transferred{.count = count, .cursors = cursorsChanged};
      co_return transferred;
    }

    json::value identityJson(const LegacyCodec::Identity& identity) {
      json::object out;
      out["version"] = identity.version;
      out["binary"] = identity.binary;
      out["sha256"] = identity.sha256;
      return json::value(std::move(out));
    }
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const SnapshotIdentity& value) {
    out = Writer()
            .required("sha256", value.sha256)
            .required("size", value.size)
            .required("device", value.device)
            .required("inode", value.inode)
            .required("mtime_ns", value.mtime_ns)
            .required("wal", value.wal)
            .required("drain_boundary", value.drain_boundary)
            .done();
  }

  SnapshotIdentity tag_invoke(const json::value_to_tag<SnapshotIdentity>&, const json::value& value) {
    const Reader in(value, "SnapshotIdentity");
    return {
      .sha256 = in.required<std::string>("sha256"),
      .size = in.required<std::string>("size"),
      .device = in.required<std::string>("device"),
      .inode = in.required<std::string>("inode"),
      .mtime_ns = in.required<std::string>("mtime_ns"),
      .wal = in.required<std::string>("wal"),
      .drain_boundary = in.required<std::string>("drain_boundary"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ConversionReceipt::Source& value) {
    out = Writer()
            .required("path", value.path)
            .required("codec", value.codec)
            .required("file", value.file)
            .required("snapshot", value.snapshot)
            .required("row_count", value.row_count)
            .required("content_sha256", value.content_sha256)
            .done();
  }

  ConversionReceipt::Source tag_invoke(const json::value_to_tag<ConversionReceipt::Source>&, const json::value& value) {
    const Reader in(value, "ConversionReceipt.source");
    return {
      .path = in.required<std::string>("path"),
      .codec = in.required<std::string>("codec"),
      .file = in.required<SnapshotIdentity>("file"),
      .snapshot = in.required<SnapshotIdentity>("snapshot"),
      .row_count = in.required<double>("row_count"),
      .content_sha256 = in.required<std::string>("content_sha256"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ConversionReceipt::Destination::File& value) {
    out = Writer().required("sha256", value.sha256).required("size", value.size).done();
  }

  ConversionReceipt::Destination::File tag_invoke(const json::value_to_tag<ConversionReceipt::Destination::File>&, const json::value& value) {
    const Reader in(value, "ConversionReceipt.destination.file");
    return {.sha256 = in.required<std::string>("sha256"), .size = in.required<std::string>("size")};
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ConversionReceipt::Destination& value) {
    out = Writer()
            .required("path", value.path)
            .required("codec", value.codec)
            .required("file", value.file)
            .required("row_count", value.row_count)
            .required("content_sha256", value.content_sha256)
            .done();
  }

  ConversionReceipt::Destination tag_invoke(const json::value_to_tag<ConversionReceipt::Destination>&, const json::value& value) {
    const Reader in(value, "ConversionReceipt.destination");
    return {
      .path = in.required<std::string>("path"),
      .codec = in.required<std::string>("codec"),
      .file = in.required<ConversionReceipt::Destination::File>("file"),
      .row_count = in.required<double>("row_count"),
      .content_sha256 = in.required<std::string>("content_sha256"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ConversionReceipt::SchemaMetadata::MigrationState& value) {
    out = Writer().required("k", value.k).required("version", value.version).done();
  }

  ConversionReceipt::SchemaMetadata::MigrationState tag_invoke(const json::value_to_tag<ConversionReceipt::SchemaMetadata::MigrationState>&, const json::value& value) {
    const Reader in(value, "ConversionReceipt.schema_metadata.migration_state");
    return {.k = in.required<std::string>("k"), .version = in.required<std::string>("version")};
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ConversionReceipt::SchemaMetadata::DataVersion& value) {
    out = Writer().required("k", value.k).required("version", value.version).required("seq", value.seq).done();
  }

  ConversionReceipt::SchemaMetadata::DataVersion tag_invoke(const json::value_to_tag<ConversionReceipt::SchemaMetadata::DataVersion>&, const json::value& value) {
    const Reader in(value, "ConversionReceipt.schema_metadata.data_version");
    return {.k = in.required<std::string>("k"), .version = in.required<std::string>("version"), .seq = in.required<std::string>("seq")};
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ConversionReceipt::SchemaMetadata& value) {
    out = Writer()
            .required("source", value.source)
            .required("migration_state", value.migration_state)
            .required("data_version", value.data_version)
            .required("user_version", value.user_version)
            .required("application_id", value.application_id)
            .done();
  }

  ConversionReceipt::SchemaMetadata tag_invoke(const json::value_to_tag<ConversionReceipt::SchemaMetadata>&, const json::value& value) {
    const Reader in(value, "ConversionReceipt.schema_metadata");
    return {
      .source = in.required<json::array>("source"),
      .migration_state = in.required<ConversionReceipt::SchemaMetadata::MigrationState>("migration_state"),
      .data_version = in.required<ConversionReceipt::SchemaMetadata::DataVersion>("data_version"),
      .user_version = in.required<std::string>("user_version"),
      .application_id = in.required<std::string>("application_id"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ConversionReceipt::LastNativeRevision& value) {
    out = Writer().required("version", value.version).required("seq", value.seq).done();
  }

  ConversionReceipt::LastNativeRevision tag_invoke(const json::value_to_tag<ConversionReceipt::LastNativeRevision>&, const json::value& value) {
    const Reader in(value, "ConversionReceipt.last_native_revision");
    return {.version = in.required<std::string>("version"), .seq = in.required<std::string>("seq")};
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ConversionReceipt& value) {
    out = Writer()
            .required("schema", value.schema)
            .required("direction", value.direction)
            .required("source", value.source)
            .required("destination", value.destination)
            .required("native_schema_version", value.native_schema_version)
            .required("schema_metadata", value.schema_metadata)
            .required("last_native_revision", value.last_native_revision)
            .required("absolute_expiry_sha256", value.absolute_expiry_sha256)
            .required("transcoded_cursors", value.transcoded_cursors)
            .required("deno", identityJson(value.deno))
            .required("bun_version", value.bun_version)
            .required("completed_at", value.completed_at)
            .done();
  }

  ConversionReceipt tag_invoke(const json::value_to_tag<ConversionReceipt>&, const json::value& value) {
    const Reader in(value, "ConversionReceipt");
    const json::value deno(in.required<json::object>("deno"));
    const Reader denoIn(deno, "ConversionReceipt.deno");
    return {
      .schema = in.required<std::string>("schema"),
      .direction = in.required<std::string>("direction"),
      .source = in.required<ConversionReceipt::Source>("source"),
      .destination = in.required<ConversionReceipt::Destination>("destination"),
      .native_schema_version = in.required<double>("native_schema_version"),
      .schema_metadata = in.required<ConversionReceipt::SchemaMetadata>("schema_metadata"),
      .last_native_revision = in.required<ConversionReceipt::LastNativeRevision>("last_native_revision"),
      .absolute_expiry_sha256 = in.required<std::string>("absolute_expiry_sha256"),
      .transcoded_cursors = in.required<double>("transcoded_cursors"),
      .deno =
        LegacyCodec::Identity{
          .version = denoIn.required<std::string>("version"),
          .binary = denoIn.required<std::string>("binary"),
          .sha256 = denoIn.required<std::string>("sha256"),
        },
      .bun_version = in.required<std::string>("bun_version"),
      .completed_at = in.required<std::string>("completed_at"),
    };
  }

  namespace detail {
    rt::Task<SnapshotIdentity> withPrivateConsistentBunSnapshot(std::string sourcePath, std::string stagingParent, SnapshotRead read) {
      const std::string source = rt::path::resolve(sourcePath);
      rt::Task<SnapshotIdentity> identifying = snapshotIdentity(source);
      const SnapshotIdentity sourceIdentity = co_await std::move(identifying);
      const std::string staging = rt::fs::mkdtempSync(rt::path::join(rt::path::resolve(stagingParent), ".uos-kv-capture-"));
      std::optional<NativeSnapshotReader> reader;
      rt::Reason failure;
      try {
        rt::fs::chmodSync(staging, 0700);
        const std::string snapshot = rt::path::join(staging, "source.sqlite3");
        rt::fs::copyFileSync(source, snapshot, rt::fs::constants::COPYFILE_EXCL);
        rt::fs::chmodSync(snapshot, 0600);
        rt::Task<SnapshotIdentity> copying = privateSnapshotIdentity(snapshot, std::nullopt);
        const SnapshotIdentity identity = co_await std::move(copying);
        bool changed = identity.sha256 != sourceIdentity.sha256;
        if (!changed) {
          rt::Task<SnapshotIdentity> rereading = snapshotIdentity(source);
          const SnapshotIdentity current = co_await std::move(rereading);
          changed = !(current == sourceIdentity);
        }
        if (changed) throw rt::JsError("Source changed during snapshot");
        rt::Task<OpenedSnapshot> opening = openPrivateSnapshot(snapshot, "bun-jsc-v1", identity);
        const OpenedSnapshot opened = co_await std::move(opening);
        reader = opened.reader;
        const NativeReader view = *reader;
        rt::Task<void> reading = read(view, opened.revision);
        co_await std::move(reading);
        reader->close();
        reader.reset();
        rt::Task<SnapshotIdentity> verifying = privateSnapshotIdentity(snapshot, identity);
        co_await std::move(verifying);
        rt::Task<SnapshotIdentity> finalIdentity = snapshotIdentity(source);
        const SnapshotIdentity after = co_await std::move(finalIdentity);
        if (!(after == sourceIdentity)) throw rt::JsError("Source changed during snapshot read");
      } catch (...) {
        failure = std::current_exception();
      }
      if (reader) reader->close();
      removeTree(staging);
      if (failure) std::rethrow_exception(failure);
      co_return sourceIdentity;
    }
  }

  rt::Task<ConversionReceipt> convertKvDatabase(ConvertOptions options) {
    if (options.direction != "deno-to-bun" && options.direction != "bun-to-deno") throw rt::JsError("Unknown conversion direction");
    const std::string source = rt::path::resolve(options.source);
    const std::string destination = rt::path::resolve(options.destination);
    const std::string receiptPath = rt::path::resolve(options.receipt);
    if (source == destination || source == receiptPath || destination == receiptPath || rt::fs::existsSync(destination) || rt::fs::existsSync(receiptPath))
      throw rt::JsError("Conversion destination and receipt must be NEW paths");
    if (options.signal) options.signal->throwIfAborted();
    rt::Task<SnapshotIdentity> identifying = snapshotIdentity(source);
    const SnapshotIdentity identity = co_await std::move(identifying);
    const std::string staging = rt::fs::mkdtempSync(rt::path::join(rt::path::dirname(destination), ".uos-kv-conversion-"));
    std::optional<NativeSnapshotReader> input;
    std::optional<rt::sqlite::Database> output;
    std::optional<LegacyCodec> codec;
    bool published = false;
    bool receiptPublished = false;
    const auto abort = [&options] {
      if (options.signal) options.signal->throwIfAborted();
    };
    std::optional<ConversionReceipt> result;
    rt::Reason failure;
    try {
      const std::string snapshot = rt::path::join(staging, "source.sqlite3");
      const std::string candidate = rt::path::join(staging, "candidate.sqlite3");
      const std::string receiptFile = rt::path::join(staging, "receipt.json");
      rt::fs::copyFileSync(source, snapshot, rt::fs::constants::COPYFILE_EXCL);
      rt::Task<SnapshotIdentity> copying = privateSnapshotIdentity(snapshot, std::nullopt);
      const SnapshotIdentity copiedIdentity = co_await std::move(copying);
      bool changed = copiedIdentity.sha256 != identity.sha256;
      if (!changed) {
        rt::Task<SnapshotIdentity> rereading = snapshotIdentity(source);
        const SnapshotIdentity current = co_await std::move(rereading);
        changed = !(current == identity);
      }
      if (changed) throw rt::JsError("Source changed during snapshot");
      const std::string sourceCodec = options.direction == "deno-to-bun" ? "deno-v8" : "bun-jsc-v1";
      const std::string destinationCodec = options.direction == "deno-to-bun" ? "bun-jsc-v1" : "deno-v8";
      rt::Task<OpenedSnapshot> opening = openPrivateSnapshot(snapshot, sourceCodec, copiedIdentity);
      const OpenedSnapshot opened = co_await std::move(opening);
      input = opened.reader;
      const NativeRevision revision = opened.revision;
      json::array schemaMetadata;
      for (const NativeRow& row : input->query("SELECT type,name,sql FROM sqlite_master ORDER BY name").all()) schemaMetadata.emplace_back(rt::sqlite::toJson(row));
      if (options.signal) {
        rt::Task<LegacyCodec> openingCodec = legacy_codec::openLegacyCodec(options.deno, options.denoSha256, staging, *options.signal);
        codec = co_await std::move(openingCodec);
      } else {
        rt::Task<LegacyCodec> openingCodec = legacy_codec::openLegacyCodec(options.deno, options.denoSha256, staging);
        codec = co_await std::move(openingCodec);
      }
      rt::sqlite::DatabaseOptions outputOptions;
      outputOptions.create = true;
      outputOptions.strict = true;
      outputOptions.safeIntegers = true;
      output.emplace(candidate, outputOptions);
      output->run("PRAGMA journal_mode=DELETE");
      output->run("BEGIN IMMEDIATE");
      for (const std::string_view statement : native_schema::NATIVE_DDL) output->run(statement);
      output->run("INSERT INTO migration_state(k,version) VALUES(0,3)");
      std::vector<rt::sqlite::Binding> revisionParameters;
      revisionParameters.emplace_back(revision.k);
      revisionParameters.emplace_back(revision.version);
      revisionParameters.emplace_back(revision.seq);
      output->run("INSERT INTO data_version(k,version,seq) VALUES(?,?,?)", revisionParameters);
      if (destinationCodec == "bun-jsc-v1") {
        output->run("CREATE TABLE uos_kv_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID");
        std::vector<rt::sqlite::Binding> versionParameters;
        versionParameters.emplace_back(std::string(rt::bun::version));
        output->run("INSERT INTO uos_kv_metadata(key,value) VALUES('codec','bun-jsc-v1'),('created_bun_version',?)", versionParameters);
      }
      rt::Task<Transferred> transferring = transferRows(*input, *output, *codec, options, revision);
      const Transferred transferred = co_await std::move(transferring);
      rt::Task<void> closing = codec->close();
      co_await std::move(closing);
      abort();
      output->run("COMMIT");
      native_schema::verifyNativeSchema(nativeReader(*output), destinationCodec);
      const std::string sourceDigest = digestRows(*input);
      const std::string destinationDigest = digestRows(nativeReader(*output));
      const std::string expiryDigest = digestRows(*input, true);
      if (expiryDigest != digestRows(nativeReader(*output), true)) throw rt::JsError("Absolute expiration or raw key changed during conversion");
      input->close();
      input.reset();
      output->close(true);
      output.reset();
      rt::Task<SnapshotIdentity> verifying = privateSnapshotIdentity(snapshot, copiedIdentity);
      co_await std::move(verifying);
      rt::Task<SnapshotIdentity> finalIdentity = snapshotIdentity(source);
      const SnapshotIdentity current = co_await std::move(finalIdentity);
      if (!(current == identity)) throw rt::JsError("Source changed before conversion publication");
      rt::Task<std::string> candidateDigesting = legacy_codec::fileDigest(candidate);
      std::string candidateSha256 = co_await std::move(candidateDigesting);
      const std::string candidateSize = std::to_string(rt::fs::lstatSync(candidate).size);
      ConversionReceipt receipt{
        .schema = "uos-kv-runtime-conversion-v1",
        .direction = options.direction,
        .source =
          ConversionReceipt::Source{
            .path = source,
            .codec = sourceCodec,
            .file = identity,
            .snapshot = identity,
            .row_count = transferred.count,
            .content_sha256 = sourceDigest,
          },
        .destination =
          ConversionReceipt::Destination{
            .path = destination,
            .codec = destinationCodec,
            .file = ConversionReceipt::Destination::File{.sha256 = std::move(candidateSha256), .size = candidateSize},
            .row_count = transferred.count,
            .content_sha256 = destinationDigest,
          },
        .native_schema_version = 3,
        .schema_metadata =
          ConversionReceipt::SchemaMetadata{
            .source = std::move(schemaMetadata),
            .migration_state = ConversionReceipt::SchemaMetadata::MigrationState{.k = "0", .version = "3"},
            .data_version =
              ConversionReceipt::SchemaMetadata::DataVersion{
                .k = std::to_string(revision.k),
                .version = std::to_string(revision.version),
                .seq = std::to_string(revision.seq),
              },
            .user_version = "0",
            .application_id = "0",
          },
        .last_native_revision = ConversionReceipt::LastNativeRevision{.version = std::to_string(revision.version), .seq = std::to_string(revision.seq)},
        .absolute_expiry_sha256 = expiryDigest,
        .transcoded_cursors = transferred.cursors,
        .deno = codec->identity,
        .bun_version = std::string(rt::bun::version),
        .completed_at = rt::Date(rt::dateNow()).toISOString(),
      };
      rt::fs::WriteFileOptions exclusive;
      exclusive.flag = "wx";
      exclusive.mode = 0600;
      rt::fs::writeFileSync(receiptFile, rt::jsonStringify(json::value_from(receipt), 2) + "\n", exclusive);
      rt::fs::chmodSync(candidate, 0600);
      syncFile(candidate);
      syncFile(receiptFile);
      abort();
      // Hard links are atomic and exclusive: even a concurrent destination creator cannot be overwritten.
      rt::fs::linkSync(candidate, destination);
      published = true;
      rt::fs::linkSync(receiptFile, receiptPath);
      receiptPublished = true;
      syncFile(rt::path::dirname(destination));
      if (rt::path::dirname(receiptPath) != rt::path::dirname(destination)) syncFile(rt::path::dirname(receiptPath));
      result = std::move(receipt);
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) {
      rt::Reason replaced;
      try {
        if (codec) {
          rt::Task<void> aborting = codec->abort();
          co_await std::move(aborting);
        }
        if (input) input->close();
        if (output) output->close(true);
        if (receiptPublished) rt::fs::rmSync(receiptPath);
        if (published) rt::fs::rmSync(destination);
      } catch (...) {
        replaced = std::current_exception();
      }
      removeTree(staging);
      std::rethrow_exception(replaced ? replaced : failure);
    }
    removeTree(staging);
    ConversionReceipt receipt = std::move(*result);
    co_return receipt;
  }
}
