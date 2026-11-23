// Differential scenarios for uos/rt/ordered.hpp. Each scenario's expected trace was
// produced by Bun 1.4 running the same operations on a JS Map or Set, including
// mutation during for...of and forEach.
#include "uos/rt/ordered.hpp"

#include "uos/rt/number.hpp"
#include "uos/rt/test.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace rt = uos::rt;
using namespace std::literals;

namespace {
  struct Scenario {
    std::string_view script;
    std::string_view trace;
  };

  // @@TABLES-BEGIN
  const Scenario kMapNumeric[] = {
    {"s 1 10;s 2 20;s 3 30;s 1 11;k;v;z"sv, "k:[1,2,3]|v:[11,20,30]|z:3"sv},
    {"s 1 10;s 2 20;d 1;s 1 12;k"sv, "d:true|k:[2,1]"sv},
    {"s NaN 1;g NaN;h NaN;s NaN 2;k;z;d NaN;z"sv, "g:1|h:true|k:[NaN]|z:1|d:true|z:0"sv},
    {"s -0 1;g 0;h -0;h 0;k;s 0 2;g -0;k"sv, "g:1|h:true|h:true|k:[0]|g:2|k:[0]"sv},
    {"s 1 1;s 2 2;s 3 3;i@0/d 2;k"sv, "1=1|d:true|3=3|k:[1,3]"sv},
    {"s 1 1;s 2 2;s 3 3;i@0/s 4 4@3/s 5 5;k"sv, "1=1|2=2|3=3|4=4|5=5|k:[1,2,3,4,5]"sv},
    {"s 1 1;s 2 2;s 3 3;i@1/d 1/s 1 9;k"sv, "1=1|2=2|d:true|3=3|1=9|k:[2,3,1]"sv},
    {"s 1 1;s 2 2;i@0/d 1/s 1 1@1/d 2/s 2 2"sv, "1=1|d:true|2=2|d:true|1=1|2=2"sv},
    {"s 1 1;s 2 2;s 3 3;i@0/c@1/s 9 9;k;z"sv, "1=1|k:[]|z:0"sv},
    {"s 1 1;s 2 2;s 3 3;i@0/c/s 7 7/s 8 8;k"sv, "1=1|7=7|8=8|k:[7,8]"sv},
    {"s 1 1;s 2 2;s 3 3;i@1/s 3 33/s 1 11;k;v"sv, "1=1|2=2|3=33|k:[1,2,3]|v:[11,2,33]"sv},
    {"s 1 1;s 2 2;s 3 3;f@0/d 3/s 4 4;k"sv, "f1=1|d:true|f2=2|f4=4|k:[1,2,4]"sv},
    {"s 1 1;s 2 2;s 3 3;i@0/d 1/d 2/d 3;z;k"sv, "1=1|d:true|d:true|d:true|z:0|k:[]"sv},
    {"s 1 1;s 2 2;s 3 3;i@2/c;z;i;k"sv, "1=1|2=2|3=3|z:0|k:[]"sv},
    {"s 1 1;i@0/s 2 2@1/s 3 3@2/s 4 4;k"sv, "1=1|2=2|3=3|4=4|k:[1,2,3,4]"sv},
    {"i;s 5 5;i@0/d 5;z"sv, "5=5|d:true|z:0"sv},
    {"s 1 1;s 2 2;s 3 3;s 4 4;d 2;d 3;s 2 22;k;i@0/d 4/s 3 3;k"sv, "d:true|d:true|k:[1,4,2]|1=1|d:true|2=22|3=3|k:[1,2,3]"sv},
    {"s Infinity 1;s -Infinity 2;s 2.5 3;k;g 2.5;d Infinity;k"sv, "k:[Infinity,-Infinity,2.5]|g:3|d:true|k:[-Infinity,2.5]"sv},
    {"d 2.5;d 4;g 2;d -Infinity;s 4 97;g -0;d 3;k;k;z"sv, "d:false|d:false|g:undefined|d:false|g:undefined|d:false|k:[4]|k:[4]|z:1"sv},
    {"k;s NaN 19;s 2.5 7;s 2.5 6;s 0 86;h 0;i@5/g -Infinity@1/z/d 4;s Infinity 85;i;s 4 92;s 2 83;d 5;k;z"sv, "k:[]|h:true|NaN=19|2.5=6|z:3|d:false|0=86|NaN=19|2.5=6|0=86|Infinity=85|d:false|k:[NaN,2.5,0,Infinity,4,2]|z:6"sv},
    {"d 2.5;f@0/h Infinity/s Infinity 43@5/h 3;h 1;k;k;z"sv, "d:false|h:false|k:[]|k:[]|z:0"sv},
    {"s 2 71;i@3/g 2@5/s 5 40;d -0;g -Infinity;s 4 81;k;s 4 11;h 4;s 3 82;f@3/d 2.5;h 2;s 4 64;k;z"sv, "2=71|d:false|g:undefined|k:[2,4]|h:true|f2=71|f4=11|f3=82|h:true|k:[2,4,3]|z:3"sv},
    {"s Infinity 17;h 2;s 2.5 45;i;s 5 36;z;g -Infinity;k;z"sv, "h:false|Infinity=17|2.5=45|z:3|g:undefined|k:[Infinity,2.5,5]|z:3"sv},
    {"d NaN;k;d -Infinity;d 1;i;s 0 60;k;z"sv, "d:false|k:[]|d:false|d:false|k:[0]|z:1"sv},
    {"d -0;i;s Infinity 2;s 1 8;i@2/d 1;s 2.5 85;f@0/s 5 58@5/s 2 92@0/d 4/s -Infinity 26;h 1;i;k;z"sv, "d:false|Infinity=2|1=8|fInfinity=2|d:false|f1=8|f2.5=85|f-Infinity=26|h:true|Infinity=2|1=8|2.5=85|-Infinity=26|k:[Infinity,1,2.5,-Infinity]|z:4"sv},
    {"d 3;i;s -Infinity 13;s NaN 19;d -0;s -Infinity 10;d 3;s 0 5;f@1/h 5/g -0;k;z"sv, "d:false|d:false|d:false|f-Infinity=10|fNaN=19|h:false|g:5|f0=5|k:[-Infinity,NaN,0]|z:3"sv},
    {"s 2 21;z;s Infinity 70;s Infinity 24;z;s 2 76;s 2 51;g NaN;d Infinity;i@1/d 3/s -Infinity 78;k;z"sv, "z:1|z:2|g:undefined|d:true|2=51|k:[2]|z:1"sv},
    {"f;c;s 2.5 18;f@1/d 3/h 0@2/s Infinity 54/d 2;s -0 59;d 0;i@1/s NaN 16@1/s -0 37;s 3 28;k;z"sv, "f2.5=18|d:true|2.5=18|k:[2.5,3]|z:2"sv},
    {"d -0;f;i;s 5 30;s 2.5 34;c;s 1 85;i@5/h NaN;g 2;s 2 41;k;z"sv, "d:false|1=85|g:undefined|k:[1,2]|z:2"sv},
    {"z;i@4/s NaN 73;s 2.5 66;s 5 54;s Infinity 9;s -Infinity 53;s -0 41;z;f;d -Infinity;k;z"sv, "z:0|z:5|f2.5=66|f5=54|fInfinity=9|f-Infinity=53|f0=41|d:true|k:[2.5,5,Infinity,0]|z:4"sv},
    {"s 5 66;s -0 41;s 1 33;d 5;z;i@1/g 2/d -Infinity@5/s -0 75/h Infinity;i@4/s -Infinity 13@3/s 5 57/c@5/s 0 15;i@1/d Infinity@3/d Infinity;h NaN;k;z"sv, "d:true|z:2|0=41|1=33|g:undefined|d:false|0=41|1=33|0=41|1=33|d:false|h:false|k:[0,1]|z:2"sv},
    {"d 1;s NaN 37;h -Infinity;i@1/s -Infinity 27;c;s Infinity 40;s NaN 1;g 0;s -0 82;g 2.5;d 1;k;z"sv, "d:false|h:false|NaN=37|g:undefined|g:undefined|d:false|k:[Infinity,NaN,0]|z:3"sv},
    {"s 3 76;z;s 1 7;s Infinity 37;i@3/g 1@1/g 2.5;f@5/z/s 2 97@2/s -0 55;k;z"sv, "z:1|3=76|1=7|g:undefined|Infinity=37|f3=76|f1=7|fInfinity=37|f0=55|k:[3,1,Infinity,0]|z:4"sv},
    {"i@3/d Infinity/s 4 59@0/h 2.5/s Infinity 95@0/d Infinity/g 2;h 1;s 3 12;s Infinity 89;s 2 25;i@0/z@3/g 3;s 1 33;s 0 82;s Infinity 26;s NaN 40;s 3 23;s 0 37;s -Infinity 63;k;z"sv, "h:false|3=12|z:3|Infinity=89|2=25|k:[3,Infinity,2,1,0,NaN,-Infinity]|z:7"sv},
    {"s -0 47;s -0 0;z;s NaN 99;i@2/d 2/s -Infinity 71;s 2.5 25;d 3;i@5/s 2.5 38@4/s -Infinity 28/s 1 84;v;c;g 2.5;v;k;z"sv, "z:1|0=0|NaN=99|d:false|0=0|NaN=99|2.5=25|v:[0,99,25]|g:undefined|v:[]|k:[]|z:0"sv},
    {"s 2.5 63;s -0 24;s 4 72;f@1/c@4/h 4/s 3 23@5/s -0 7;k;z"sv, "f2.5=63|f0=24|k:[]|z:0"sv},
    {"c;s -0 54;f@2/s 3 49/s 3 8;g 3;s 0 58;g NaN;s 1 74;k;z"sv, "f0=54|g:undefined|g:undefined|k:[0,1]|z:2"sv},
    {"h NaN;z;s 4 89;g 2.5;s -Infinity 68;k;z"sv, "h:false|z:0|g:undefined|k:[4,-Infinity]|z:2"sv},
    {"i@5/s Infinity 15/s Infinity 79;h 2.5;z;s 2 28;s 3 5;s NaN 82;s 1 47;z;d -0;k;z"sv, "h:false|z:0|z:4|d:false|k:[2,3,NaN,1]|z:4"sv},
    {"s 3 85;h 3;d 5;s 0 56;d 2;h 4;d 2;c;k;z"sv, "h:true|d:false|d:false|h:false|d:false|k:[]|z:0"sv},
    {"s 4 70;g NaN;d 1;i@0/s NaN 48;s NaN 89;k;z"sv, "g:undefined|d:false|4=70|NaN=48|k:[4,NaN]|z:2"sv},
    {"i@5/s 2 19;h 0;s 0 46;i@5/d 5/s NaN 2@3/s 3 19;s Infinity 22;k;z"sv, "h:false|0=46|k:[0,Infinity]|z:2"sv},
    {"s 2.5 40;s -0 39;f@2/d 3/s Infinity 26@1/h NaN@2/c/c;d 2;d 3;z;i@1/s 2 85@1/s 2 21/s 1 30@0/d 2.5/s 4 37;k;z"sv, "f2.5=40|f0=39|h:false|d:false|d:false|z:2|2.5=40|d:true|0=39|4=37|2=21|1=30|k:[0,4,2,1]|z:4"sv},
    {"c;s -Infinity 94;s 1 49;s -0 72;d Infinity;k;z"sv, "d:false|k:[-Infinity,1,0]|z:3"sv},
    {"s 0 7;s -0 7;d -Infinity;d 4;v;i@1/c@5/c@0/s 4 81;c;s NaN 1;s 1 35;v;i@0/d 0@4/s 4 32@0/g -0;i@2/g 0/s 1 56;k;z"sv, "d:false|d:false|v:[7]|0=7|4=81|v:[1,35]|NaN=1|g:undefined|1=35|NaN=1|1=35|k:[NaN,1]|z:2"sv},
    {"s -Infinity 69;d 0;s -Infinity 97;h Infinity;s 1 5;s 2.5 93;s 0 34;s 1 98;i@3/z/s 2.5 47@0/g 4@3/s -Infinity 19/s 3 63;d -Infinity;s 5 62;k;z"sv, "d:false|h:false|-Infinity=97|g:undefined|1=98|2.5=93|0=34|3=63|d:true|k:[1,2.5,0,3,5]|z:5"sv},
    {"s 2 43;s 1 90;i;g -0;k;z"sv, "2=43|1=90|g:undefined|k:[2,1]|z:2"sv},
    {"d 0;d NaN;h 5;d 5;k;z"sv, "d:false|d:false|h:false|d:false|k:[]|z:0"sv},
    {"f@5/c@5/s -Infinity 33/s -Infinity 20;d 4;v;s 2.5 1;d 0;k;z"sv, "d:false|v:[]|d:false|k:[2.5]|z:1"sv},
    {"i@3/s Infinity 64@1/d -0/c@0/s 2 29;s 3 68;i@2/s 4 98/s Infinity 54@0/s 4 63@3/g Infinity;d 3;s 3 22;i@3/s 2.5 55/s 4 15@4/d 2.5/s 5 9@3/d -Infinity/s 5 32;k;z"sv, "3=68|4=63|d:true|4=63|3=22|k:[4,3]|z:2"sv},
    {"s 2 97;s 3 82;z;d 4;s 4 53;s 1 77;i@3/h 3/s NaN 24@1/h 1;f@1/s -0 66@2/s 3 18/s -Infinity 97@0/s 5 37/s 3 84;d -Infinity;d 2;f@5/s 1 28/s Infinity 36;i;i;k;z"sv, "z:2|d:false|2=97|3=82|h:true|4=53|1=77|h:true|NaN=24|f2=97|f3=84|f4=53|f1=77|fNaN=24|f5=37|f0=66|f-Infinity=97|d:true|d:true|f3=18|f4=53|f1=77|fNaN=24|f5=37|f0=66|fInfinity=36|3=18|4=53|1=28|NaN=24|5=37|0=66|Infinity=36|3=18|4=53|1=28|NaN=24|5=37|0=66|Infinity=36|k:[3,4,1,NaN,5,0,Infinity]|z:7"sv},
    {"d 5;d 0;s 2.5 69;g -0;h NaN;d 2;i;s 1 53;k;z"sv, "d:false|d:false|g:undefined|h:false|d:false|2.5=69|k:[2.5,1]|z:2"sv},
    {"s 1 69;s 3 41;i;s NaN 70;s NaN 54;k;z"sv, "1=69|3=41|k:[1,3,NaN]|z:3"sv},
    {"s 2.5 55;d 2;s Infinity 74;s -0 89;i;s 2.5 29;s 1 19;s 3 92;i;h 5;s 2.5 45;k;z"sv, "d:false|2.5=55|Infinity=74|0=89|2.5=29|Infinity=74|0=89|1=19|3=92|h:false|k:[2.5,Infinity,0,1,3]|z:5"sv},
    {"d -0;i@4/s -0 60/h 0;s -0 15;z;i@3/s 2 79@1/s 0 53;d Infinity;k;z"sv, "d:false|z:1|0=15|d:false|k:[0]|z:1"sv},
    {"d 4;f;d -0;s 5 32;d 2.5;g 1;i@0/d 2@5/h NaN;s 5 76;d NaN;i@1/s 2.5 25@4/s 1 50@1/s 3 31/s 3 61;d 4;k;z"sv, "d:false|d:false|d:false|g:undefined|5=32|d:false|d:false|5=76|d:false|k:[5]|z:1"sv},
    {"z;i@3/s 5 71;i@0/g 1/d 4@5/s Infinity 3;i@3/s -Infinity 64;f@2/s -0 22/d NaN;s 2 45;d -0;h 2;i@1/d 5/d 1;d NaN;k;z"sv, "z:0|d:false|h:true|2=45|d:false|k:[2]|z:1"sv},
    {"i@2/s 2.5 7/s 5 94@1/s 0 68;s 2 17;v;d -0;d 1;h NaN;i@4/d 2.5/h 2;g NaN;g 0;s 1 39;k;z"sv, "v:[17]|d:false|d:false|h:false|2=17|g:undefined|g:undefined|k:[2,1]|z:2"sv},
    {"z;s -Infinity 14;s -0 40;c;i;k;s NaN 98;i@4/s 1 53/d 2.5@0/s 2 46@2/s 5 30;k;z"sv, "z:0|k:[]|NaN=98|2=46|k:[NaN,2]|z:2"sv},
    {"g 3;f;s 5 4;s 1 75;g -Infinity;k;f@4/s NaN 38/s NaN 39@5/d 0/h 0;h 1;s Infinity 15;d 0;k;z"sv, "g:undefined|g:undefined|k:[5,1]|f5=4|f1=75|h:true|d:false|k:[5,1,Infinity]|z:3"sv},
    {"z;s 5 57;h Infinity;h 5;k;z"sv, "z:0|h:false|h:true|k:[5]|z:1"sv},
    {"d 2.5;d -Infinity;i@2/s -0 0@1/z;f;s 1 65;d 2;z;d -0;s NaN 85;s 2.5 89;k;z"sv, "d:false|d:false|d:false|z:1|d:false|k:[1,NaN,2.5]|z:3"sv},
    {"s 2.5 46;d -0;i;s -Infinity 72;i@5/s -0 30@0/s NaN 24;h 1;h 1;i@1/g -0/s 3 20@1/g 1/d 0;s Infinity 67;d 4;s 0 73;k;z"sv, "d:false|2.5=46|2.5=46|-Infinity=72|NaN=24|h:false|h:false|2.5=46|-Infinity=72|g:undefined|d:false|NaN=24|d:false|k:[2.5,-Infinity,NaN,Infinity,0]|z:5"sv},
    {"s 2 26;d 4;s -Infinity 5;i;s 3 34;s 4 98;s 0 77;s 2 98;k;z"sv, "d:false|2=26|-Infinity=5|k:[2,-Infinity,3,4,0]|z:5"sv},
    {"i;h 4;d NaN;f@4/g -Infinity@1/s 5 74@3/s 2 62;z;k;z"sv, "h:false|d:false|z:0|k:[]|z:0"sv},
    {"d 2.5;d 2;s 3 7;h 1;i@2/g -Infinity/h -Infinity@5/h 3/d 2@0/g -0;f@2/s NaN 16/s 5 31;i@0/z/d Infinity@3/z/d -Infinity;i@2/d 1@2/d NaN/s 2.5 55@0/s -0 82;i@4/s 2 24/s -0 70;d Infinity;d 0;s 3 92;k;z"sv, "d:false|d:false|h:false|3=7|g:undefined|f3=7|3=7|z:1|d:false|3=7|0=82|3=7|0=82|d:false|d:true|k:[3]|z:1"sv},
    {"s NaN 57;d 2.5;s 5 47;s -Infinity 62;z;s Infinity 29;s 4 79;i@1/d 2@3/s 3 54@3/s NaN 0/s 2 10;h 5;s 5 33;c;d -0;d -Infinity;k;z"sv, "d:false|z:3|NaN=57|5=47|d:false|-Infinity=62|Infinity=29|4=79|2=10|h:true|d:false|d:false|k:[]|z:0"sv},
    {"h 4;s -0 88;s NaN 40;d -0;d 4;d 4;s Infinity 25;z;s -0 53;k;z"sv, "h:false|d:true|d:false|d:false|z:2|k:[NaN,Infinity,0]|z:3"sv},
    {"s -Infinity 1;s NaN 32;s 5 48;s -Infinity 32;d 3;s 0 80;h 3;d 1;i@5/h 0/d Infinity@1/z@4/d 3/h NaN;s Infinity 80;s 1 79;k;z"sv, "d:false|h:false|d:false|-Infinity=32|NaN=32|z:4|5=48|0=80|k:[-Infinity,NaN,5,0,Infinity,1]|z:6"sv},
    {"z;i@5/s 2.5 64@1/g 3;s 0 55;z;d NaN;d 1;c;k;z"sv, "z:0|z:1|d:false|d:false|k:[]|z:0"sv},
    {"s Infinity 38;s 4 62;i@0/d 2.5/h 0@3/s -Infinity 75/s -Infinity 42;c;f@3/d 1/d 3;v;g 2.5;s 0 64;d -0;k;z"sv, "Infinity=38|d:false|h:false|4=62|v:[]|g:undefined|d:true|k:[]|z:0"sv},
    {"s Infinity 86;s 0 47;k;d 4;z;k;z"sv, "k:[Infinity,0]|d:false|z:2|k:[Infinity,0]|z:2"sv},
    {"i@4/h 3@3/h -Infinity/s -0 83;i@4/d 0@3/s 2 86@5/s -Infinity 32;s -0 35;s Infinity 22;f@3/s 0 46/d Infinity;d NaN;i@5/h 1@2/d Infinity;s 1 5;i@1/d Infinity@4/c;s 0 37;s 1 88;i@2/s 1 95/d 5@2/s 5 73/g 4@3/d -Infinity;s NaN 1;k;z"sv, "f0=35|fInfinity=22|d:false|0=35|Infinity=22|0=35|Infinity=22|d:true|1=5|0=37|1=88|k:[0,1,NaN]|z:3"sv},
    {"s 2 94;h 1;s -0 19;h 2;c;s 2 66;k;z"sv, "h:false|h:true|k:[2]|z:1"sv},
    {"i@0/s 1 34/d 3;v;g 2.5;s 0 57;s Infinity 12;h -0;d -Infinity;d 2.5;g 5;s 0 67;c;s 2.5 61;s 3 23;k;z"sv, "v:[]|g:undefined|h:true|d:false|d:false|g:undefined|k:[2.5,3]|z:2"sv},
    {"h 2.5;s -0 0;i@0/s 2.5 68;i@5/h 4;s 2.5 26;z;i@1/z;k;z"sv, "h:false|0=0|2.5=68|0=0|2.5=68|z:2|0=0|2.5=26|z:2|k:[0,2.5]|z:2"sv},
    {"s Infinity 3;h 5;z;s 5 13;g 1;s NaN 42;d -0;k;z"sv, "h:false|z:1|g:undefined|d:false|k:[Infinity,5,NaN]|z:3"sv},
    {"i;s 2.5 98;s Infinity 7;h -0;s -Infinity 23;k;z"sv, "h:false|k:[2.5,Infinity,-Infinity]|z:3"sv},
  };
  const Scenario kSetNumeric[] = {
    {"a 1;a 2;a 1;k;z"sv, "k:[1,2]|z:2"sv},
    {"a NaN;a NaN;h NaN;z"sv, "h:true|z:1"sv},
    {"a -0;h 0;k;a 0;z"sv, "h:true|k:[0]|z:1"sv},
    {"a 1;a 2;a 3;i@0/d 2/a 4;k"sv, "1|d:true|3|4|k:[1,3,4]"sv},
    {"a 1;a 2;i@0/d 1/a 1@1/d 2/a 2"sv, "1|d:true|2|d:true|1|2"sv},
    {"a 1;a 2;a 3;i@0/c/a 5;k"sv, "1|5|k:[5]"sv},
    {"a 1;a 2;a 3;f@1/d 3/a 9;k"sv, "f1|f2|d:true|f9|k:[1,2,9]"sv},
    {"d 1;a -0;a NaN;a 3;a -0;k;z"sv, "d:false|k:[0,NaN,3]|z:3"sv},
    {"i;h 2.5;i@0/a 4/d Infinity@4/h NaN/a -0@3/a 4/a 4;i@0/d 2.5/a 3@5/d NaN/a 2;a Infinity;v;i@5/a NaN;d 1;d NaN;k;z"sv, "h:false|v:[Infinity]|Infinity|d:false|d:false|k:[Infinity]|z:1"sv},
    {"a 4;a -0;d 3;d 3;f@1/a NaN/a 5;i@1/a 4;k;z"sv, "d:false|d:false|f4|f0|fNaN|f5|4|0|NaN|5|k:[4,0,NaN,5]|z:4"sv},
    {"a -Infinity;d -Infinity;d 2.5;i;a -0;c;d 2;i@3/a -0/a Infinity@1/d Infinity/a 4@0/h Infinity;h 2;a -Infinity;k;z"sv, "d:true|d:false|d:false|h:false|k:[-Infinity]|z:1"sv},
    {"d -Infinity;a 2.5;h -Infinity;i@4/d NaN/a 1@5/z@4/a NaN/a 2;h 2;a 0;d 5;a 4;i@1/z;a 2;a 4;k;k;z"sv, "d:false|h:false|2.5|h:false|d:false|2.5|0|z:3|4|k:[2.5,0,4,2]|k:[2.5,0,4,2]|z:4"sv},
    {"i@2/h 1@4/c;i;a 4;h Infinity;c;a 0;d 2.5;i@1/a 2.5@5/h -0;f;f@3/a 2.5/a 2;a 4;k;z"sv, "h:false|d:false|0|f0|f0|k:[0,4]|z:2"sv},
    {"a 2;c;a 5;a -Infinity;a 2.5;a 2;a -Infinity;d 5;f@0/d 2@2/a 3/a 1;c;k;z"sv, "d:true|f-Infinity|d:true|f2.5|k:[]|z:0"sv},
    {"a 2.5;c;a 2;i@4/a 0@1/d -Infinity/a 2.5;a -0;c;a NaN;v;h 4;a 3;v;k;z"sv, "2|v:[NaN]|h:false|v:[NaN,3]|k:[NaN,3]|z:2"sv},
    {"i@1/h NaN/h 3@5/h 2@2/d 2/a 4;k;h 2;d 3;d 2;f@1/d 3/h 3@0/d 0@1/d NaN;a 2.5;h 2;i;k;z"sv, "k:[]|h:false|d:false|d:false|h:false|2.5|k:[2.5]|z:1"sv},
    {"h 5;d 0;z;h 1;a 2.5;i@1/c@0/h Infinity@3/a 2/z;h 5;a 3;i@4/a -0/d 4@2/h 1/a 4;d 0;d 0;a 2.5;k;z"sv, "h:false|d:false|z:0|h:false|2.5|h:false|h:false|2.5|3|d:false|d:false|k:[2.5,3]|z:2"sv},
    {"i;a 4;a 5;a 2.5;i@5/h NaN/a NaN@2/c/d NaN;h NaN;d 2.5;k;z"sv, "4|5|2.5|d:false|h:false|d:false|k:[]|z:0"sv},
    {"i@0/h Infinity/z@5/a 5;d 2;a 1;a NaN;k;z"sv, "d:false|k:[1,NaN]|z:2"sv},
    {"h 2.5;i@2/c@3/a 3@2/a NaN/h 4;z;a 5;c;k;z"sv, "h:false|z:0|k:[]|z:0"sv},
    {"d 3;a 2.5;a 1;d 5;h 2;a NaN;a 0;d NaN;a 1;a -0;f@4/d 4/a 5@1/a 1@0/a 2;k;z"sv, "d:false|d:false|h:false|d:true|f2.5|f1|f0|f2|k:[2.5,1,0,2]|z:4"sv},
    {"a 2.5;a 2;c;a 2;i@3/a 0/h 2;f@5/a NaN/a -Infinity@1/d 1@0/a 0/a 5;h 4;a -0;h 4;k;z"sv, "2|f2|f0|d:false|f5|h:false|h:false|k:[2,0,5]|z:3"sv},
    {"a 1;a 2;h 2;d 2;f@4/z/a 3@2/a 2.5/d 0@1/d 3;k;z"sv, "h:true|d:true|f1|k:[1]|z:1"sv},
    {"z;a -0;z;a -0;h 4;a 4;a 1;a 4;c;k;z"sv, "z:0|z:1|h:false|k:[]|z:0"sv},
    {"a 4;d 2;f@5/h NaN;a 5;a 2.5;z;a 4;i@2/a 4@5/a 3;a 3;k;z"sv, "d:false|f4|z:3|4|5|2.5|k:[4,5,2.5,3]|z:4"sv},
    {"c;i@4/h -Infinity;c;i@0/d Infinity/c@3/a 5;i@5/a 0/h Infinity@0/a 5/d 1@0/h 1;a NaN;k;z"sv, "k:[NaN]|z:1"sv},
    {"d 1;h -Infinity;a 0;a Infinity;k;z"sv, "d:false|h:false|k:[0,Infinity]|z:2"sv},
    {"a 2;a -0;z;d 1;h 0;h -0;a 0;a -Infinity;d 0;h 5;h -0;z;i@4/h 1/d -0;k;z"sv, "z:2|d:false|h:true|h:true|d:true|h:false|h:false|z:2|2|-Infinity|k:[2,-Infinity]|z:2"sv},
    {"a NaN;d 5;a -Infinity;a 4;a 0;k;z"sv, "d:false|k:[NaN,-Infinity,4,0]|z:4"sv},
    {"d 4;i@2/a 4/d 5@5/a 2.5/a NaN@3/d 3;d 4;a 1;d 2;k;z"sv, "d:false|d:false|d:false|k:[1]|z:1"sv},
    {"a Infinity;a 2.5;i;z;f@5/d 2.5;d 5;h 2.5;d 0;k;z"sv, "Infinity|2.5|z:2|fInfinity|f2.5|d:false|h:true|d:false|k:[Infinity,2.5]|z:2"sv},
    {"i@0/d 2.5@0/z;a -Infinity;i;a 2;k;z"sv, "-Infinity|k:[-Infinity,2]|z:2"sv},
    {"a Infinity;a 4;d 5;a 0;i@5/a 4/h 4@0/c/z@3/d -0/a 2.5;d 3;f@1/a 2.5@1/a 3/a 2@5/d 5/a -Infinity;a 2;a 2;a 4;k;z"sv, "d:false|Infinity|z:0|d:false|k:[2,4]|z:2"sv},
    {"a 3;f;d NaN;d -0;c;d 2;a 5;a 2.5;k;z"sv, "f3|d:false|d:false|d:false|k:[5,2.5]|z:2"sv},
    {"a Infinity;h 1;i@4/h -0;c;d Infinity;a 5;a 0;h NaN;h 3;a 5;k;z"sv, "h:false|Infinity|d:false|h:false|h:false|k:[5,0]|z:2"sv},
    {"h 4;i;h 2;a 1;z;a -0;a -Infinity;a 1;h Infinity;i;k;z"sv, "h:false|h:false|z:1|h:false|1|0|-Infinity|k:[1,0,-Infinity]|z:3"sv},
    {"a -0;a Infinity;f@4/z@5/a Infinity/h 2.5@5/a NaN/d 1;i@2/a 5@0/a 4/h 0;h 2;a Infinity;f;a NaN;i;i;d 5;k;z"sv, "f0|fInfinity|0|h:true|Infinity|4|5|h:false|f0|fInfinity|f4|f5|0|Infinity|4|5|NaN|0|Infinity|4|5|NaN|d:true|k:[0,Infinity,4,NaN]|z:4"sv},
  };
  const Scenario kMapString[] = {
    {"s a 1;s b 2;s a 3;k;v"sv, "k:[a,b]|v:[3,2]"sv},
    {"s 10 1;s 2 2;s a 3;s 1 4;k"sv, "k:[10,2,a,1]"sv},
    {"s __proto__ 1;g __proto__;k"sv, "g:1|k:[__proto__]"sv},
    {"s a 1;s b 2;s c 3;i@0/d b/s d 4@2/d a;k"sv, "a=1|d:true|c=3|d=4|d:true|k:[c,d]"sv},
    {"h 10;f@4/d a/d b@3/z/s d 77@4/s d 39;s __proto__ 86;i@0/g e/s b 90;s __proto__ 34;d __proto__;i;s 2 63;s b 44;f@3/g d@5/c/d __proto__@2/s e 72/d d;z;d d;h 10;k;z"sv, "h:false|__proto__=86|g:undefined|b=90|d:true|b=90|fb=44|f2=63|z:2|d:false|h:false|k:[b,2]|z:2"sv},
    {"f@4/d b@1/c@0/s d 14;s __proto__ 68;i@1/s c 77/s 10 55;g e;i;i@1/s __proto__ 85@3/s __proto__ 25/s a 58;k;z"sv, "__proto__=68|g:undefined|__proto__=68|__proto__=68|k:[__proto__]|z:1"sv},
    {"s d 46;d 10;s d 90;f@0/g d/s d 34@5/s c 32;s a 50;g a;s __proto__ 32;h d;k;h 2;k;z"sv, "d:false|fd=90|g:90|g:50|h:true|k:[d,a,__proto__]|h:false|k:[d,a,__proto__]|z:3"sv},
    {"g 2;i;d e;k;f@5/s 2 19@4/s d 47@5/g 10/d c;i@0/g 10/s 10 40@5/s 2 79;s d 43;z;d __proto__;s __proto__ 31;k;z"sv, "g:undefined|d:false|k:[]|z:1|d:false|k:[d,__proto__]|z:2"sv},
    {"s d 45;i;s b 70;d __proto__;k;z"sv, "d=45|d:false|k:[d,b]|z:2"sv},
    {"h 10;v;v;h e;k;z"sv, "h:false|v:[]|v:[]|h:false|k:[]|z:0"sv},
    {"f@4/s e 22;f@3/s 10 97/s d 13@3/s a 5/s c 40@0/z;z;g d;s e 52;z;v;s c 42;k;z"sv, "z:0|g:undefined|z:1|v:[52]|k:[e,c]|z:2"sv},
    {"i;g e;h 10;s b 63;i;d 2;i@2/s __proto__ 2/d 2;k;z"sv, "g:undefined|h:false|b=63|d:false|b=63|k:[b]|z:1"sv},
    {"s 10 17;s a 5;d 2;k;h 2;d b;g d;i@2/s c 83/d a@1/g a/s 2 2;i@2/s __proto__ 60/s c 9@5/d e;f@2/s b 81;d __proto__;k;z"sv, "d:false|k:[10,a]|h:false|d:false|g:undefined|10=17|a=5|g:5|2=2|d:true|c=83|10=17|2=2|c=83|__proto__=60|f10=17|f2=2|fc=9|f__proto__=60|fb=81|d:true|k:[10,2,c,b]|z:4"sv},
    {"z;h a;s 10 18;i@5/s d 71/d e;h 2;s 10 36;f@1/h d/d c@4/s 10 33/s a 91@4/d b;s c 37;d b;s e 79;s 2 78;d __proto__;h __proto__;k;z"sv, "z:0|h:false|10=18|h:false|f10=36|d:false|d:false|h:false|k:[10,c,e,2]|z:4"sv},
    {"v;s e 57;s e 55;i;f;s e 58;g b;d 10;i@3/h b/d a@4/h b;s 2 75;d e;c;d d;k;z"sv, "v:[]|e=55|fe=55|g:undefined|d:false|e=58|d:true|d:false|k:[]|z:0"sv},
    {"f;d c;d e;d e;i;s __proto__ 7;d d;s 10 91;d 10;s d 0;k;z"sv, "d:false|d:false|d:false|d:false|d:true|k:[__proto__,d]|z:2"sv},
    {"z;d e;v;c;s e 20;z;g a;s c 7;s a 81;i@3/s a 30/d c;k;z"sv, "z:0|d:false|v:[]|z:1|g:undefined|e=20|c=7|a=81|k:[e,c,a]|z:3"sv},
    {"s c 62;s e 50;g b;s c 93;f;s d 64;z;s c 5;d 10;k;z"sv, "g:undefined|fc=93|fe=50|z:3|d:false|k:[c,e,d]|z:3"sv},
    {"s b 34;g d;d c;s b 74;s __proto__ 57;s d 8;i;i@3/h c@5/h e@4/s 10 14/d e;s 2 64;d a;i@3/s __proto__ 92;i@3/z/g c@4/s c 0@3/h e;k;z"sv, "g:undefined|d:false|b=74|__proto__=57|d=8|b=74|__proto__=57|d=8|d:false|b=74|__proto__=57|d=8|2=64|b=74|__proto__=92|d=8|2=64|h:false|k:[b,__proto__,d,2]|z:4"sv},
    {"k;d 2;z;i;s b 52;s a 33;f;h d;i;s 2 10;k;z"sv, "k:[]|d:false|z:0|fb=52|fa=33|h:false|b=52|a=33|k:[b,a,2]|z:3"sv},
    {"g a;g a;h __proto__;f;i@2/g 10@3/s 2 82;d d;s __proto__ 3;f@1/s b 91/s b 13@4/d __proto__/s d 67@5/s __proto__ 59/s b 57;i@3/s __proto__ 55/h b@0/s d 43/s a 2@0/z/h 2;s c 58;g 10;i;d d;k;z"sv, "g:undefined|g:undefined|h:false|d:false|f__proto__=3|__proto__=3|z:1|h:false|g:undefined|__proto__=3|c=58|d:false|k:[__proto__,c]|z:2"sv},
    {"s 10 73;d 2;d b;s a 84;s a 38;s a 71;g 10;s __proto__ 54;s c 19;s b 29;s b 39;s b 55;k;z"sv, "d:false|d:false|g:73|k:[10,a,__proto__,c,b]|z:5"sv},
    {"h c;g e;d __proto__;c;i@4/s 10 89/s 10 79;f@3/h 2/d c@5/h b/d d;k;z"sv, "h:false|g:undefined|d:false|k:[]|z:0"sv},
    {"h a;g a;g 10;s a 7;z;c;s 10 4;k;k;z"sv, "h:false|g:undefined|g:undefined|z:1|k:[10]|k:[10]|z:1"sv},
    {"s b 51;v;v;g 2;d c;s d 28;s b 40;s 2 77;s b 74;s b 31;s 10 99;i@4/s 2 89/s c 85@2/s c 59/d 10@5/s b 56;h __proto__;k;z"sv, "v:[51]|v:[51]|g:undefined|d:false|b=31|d=28|2=77|d:true|c=59|h:false|k:[b,d,2,c]|z:4"sv},
    {"z;d a;h e;v;s c 71;s b 92;h e;s e 30;i@5/s a 2/s __proto__ 60@4/h e@3/s a 69;g d;i;s __proto__ 26;k;z"sv, "z:0|d:false|h:false|v:[]|h:false|c=71|b=92|e=30|g:undefined|c=71|b=92|e=30|k:[c,b,e,__proto__]|z:4"sv},
    {"g c;f@5/d a/s e 63@0/s __proto__ 84/g 10;h __proto__;z;g a;s b 57;v;i@3/s e 24@0/z/d 2@3/g e;d 2;s c 79;k;z"sv, "g:undefined|h:false|z:0|g:undefined|v:[57]|b=57|z:1|d:false|d:false|k:[b,c]|z:2"sv},
    {"i@1/h 2/d c@5/s b 29/s d 88@0/z;d a;i@0/d __proto__@1/s d 94;s 2 95;z;d d;s 2 47;g d;s __proto__ 53;f@2/h b;z;k;z"sv, "d:false|z:1|d:false|g:undefined|f2=47|f__proto__=53|z:2|k:[2,__proto__]|z:2"sv},
    {"i@0/d 10/d 10@1/h 2;s 10 79;h b;z;s a 44;v;k;z"sv, "h:false|z:1|v:[79,44]|k:[10,a]|z:2"sv},
    {"s 2 1;h 10;d d;d 10;v;s d 94;k;z"sv, "h:false|d:false|d:false|v:[1]|k:[2,d]|z:2"sv},
    {"z;d d;d e;d __proto__;s 10 74;f@3/s 2 9@5/s 10 56;k;z"sv, "z:0|d:false|d:false|d:false|f10=74|k:[10]|z:1"sv},
    {"k;i;d 2;d b;d c;s b 80;k;z"sv, "k:[]|d:false|d:false|d:false|k:[b]|z:1"sv},
    {"d d;i;s b 33;d b;z;k;z"sv, "d:false|d:true|z:0|k:[]|z:0"sv},
    {"h d;g e;g c;f;h d;h b;d d;g c;d e;s __proto__ 65;s e 24;k;z"sv, "h:false|g:undefined|g:undefined|h:false|h:false|d:false|g:undefined|d:false|k:[__proto__,e]|z:2"sv},
  };
  const Scenario kSetString[] = {
    {"a b;a a;a 10;a 2;a b;k"sv, "k:[b,a,10,2]"sv},
    {"a a;a b;a c;i@0/d c/a d;k;z"sv, "a|d:true|b|d|k:[a,b,d]|z:3"sv},
    {"a 10;h 10;d a;d c;a a;d __proto__;a 10;a a;i;a __proto__;i@2/d e/a e;k;z"sv, "h:true|d:false|d:false|d:false|10|a|10|a|__proto__|d:false|e|k:[10,a,__proto__,e]|z:4"sv},
    {"a d;i@4/h a/a a@2/a b@0/h c/h 10;i@1/a b@4/a a/a b@3/d e/z;i@0/d c;i@0/d __proto__/d d@5/h b@1/a 2/c;d c;d a;a c;a __proto__;h a;h a;z;k;z"sv, "d|h:false|h:false|d|d|d:false|d|d:false|d:true|d:false|d:false|h:false|h:false|z:2|k:[c,__proto__]|z:2"sv},
    {"c;v;i;a 10;a a;c;f;d 2;k;z"sv, "v:[]|d:false|k:[]|z:0"sv},
    {"f;a c;d d;a c;d 10;k;z"sv, "d:false|d:false|k:[c]|z:1"sv},
    {"h b;a a;c;a 2;a e;a 10;k;z"sv, "h:false|k:[2,e,10]|z:3"sv},
    {"z;i@4/d 2/c@0/d c/c@3/a e/a b;f;h d;a c;h a;a c;z;k;z"sv, "z:0|h:false|h:false|z:1|k:[c]|z:1"sv},
    {"a a;a c;a __proto__;a c;a d;d __proto__;v;d c;a __proto__;z;i@1/h c;h 10;d 2;k;z"sv, "d:true|v:[a,c,d]|d:true|z:3|a|d|h:false|__proto__|h:false|d:false|k:[a,d,__proto__]|z:3"sv},
    {"a b;h b;a b;h a;i@0/a c/d c;k;z"sv, "h:true|h:false|b|d:true|k:[b]|z:1"sv},
    {"i;i@3/a a@1/a 10@4/z;a __proto__;a __proto__;a 10;h 10;a a;h c;i@2/a e/a e@3/a e/a a@5/a __proto__;h a;k;z"sv, "h:true|h:false|__proto__|10|a|e|h:true|k:[__proto__,10,a,e]|z:4"sv},
    {"a 2;i@5/a 10@0/a e/a 10@4/h e/a 2;a 10;i@0/a e@2/d c/h e;a b;d c;a __proto__;z;a e;a __proto__;h 2;h __proto__;k;z"sv, "2|e|10|2|e|10|d:false|h:true|d:false|z:5|h:true|h:true|k:[2,e,10,b,__proto__]|z:5"sv},
    {"a e;d 2;a 2;a a;a e;k;d c;i@2/a d;z;z;i@5/h 10;i@1/a 10@1/d __proto__@0/d c;d a;k;z"sv, "d:false|k:[e,2,a]|d:false|e|2|a|d|z:4|z:4|e|2|a|d|e|d:false|2|d:false|a|d|d:true|k:[e,2,d]|z:3"sv},
    {"a 10;z;d e;a a;c;h c;f;k;z"sv, "z:1|d:false|h:false|k:[]|z:0"sv},
    {"d d;a c;a 2;z;k;z"sv, "d:false|z:2|k:[c,2]|z:2"sv},
    {"i@4/a __proto__;a 10;i;f@5/a 10/a c@2/h b/z@3/d c;a c;h 10;k;a 2;i@0/h c/a 10;h __proto__;k;z"sv, "10|f10|h:true|k:[10,c]|10|h:true|c|2|h:false|k:[10,c,2]|z:3"sv},
    {"d c;d e;d 10;a a;a b;h a;i;c;a e;h __proto__;a 10;c;d 2;k;z"sv, "d:false|d:false|d:false|h:true|a|b|h:false|d:false|k:[]|z:0"sv},
    {"a 2;i@0/a e/a 10;h 2;h e;h c;a e;a 10;a e;h 2;h e;v;k;z"sv, "2|e|10|h:true|h:true|h:false|h:true|h:true|v:[2,e,10]|k:[2,e,10]|z:3"sv},
    {"f@1/a 10/z;d b;a a;v;h e;a 10;a c;a b;i@2/a __proto__;h e;k;z"sv, "d:false|v:[a]|h:false|a|10|c|b|__proto__|h:false|k:[a,10,c,b,__proto__]|z:5"sv},
    {"a d;f@3/a a/a 10@4/a 10;h b;a c;a c;h 10;h c;k;z"sv, "fd|h:false|h:false|h:true|k:[d,c]|z:2"sv},
    {"i;a c;h 10;a 2;i@3/a 2/a a@0/a __proto__@3/h c;h 2;d __proto__;d 10;h e;a b;h 10;k;z"sv, "h:false|c|2|__proto__|h:true|d:true|d:false|h:false|h:false|k:[c,2,b]|z:3"sv},
    {"z;i@1/h e/d e;i@2/d d;a 10;f@5/a d@0/a c/c@4/a __proto__;d c;a __proto__;k;d b;d b;a __proto__;d c;k;z"sv, "z:0|f10|d:false|k:[__proto__]|d:false|d:false|d:false|k:[__proto__]|z:1"sv},
  };
  constexpr int kVisitCap = 40;
  // @@TABLES-END

  struct Stop {};

  std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
      const std::size_t end = text.find(separator, start);
      parts.emplace_back(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
      if (end == std::string_view::npos) break;
      start = end + 1;
    }
    return parts;
  }

  double numericKey(std::string_view token) { return rt::stringToNumber(token); }
  std::string stringKey(std::string_view token) { return std::string(token); }
  std::string show(double key) { return rt::numberToString(key); }
  std::string show(const std::string& key) { return key; }
  std::string show(int value) { return std::to_string(value); }

  template <class K, bool IsMap>
  std::string run(std::string_view script, K (*parseKey)(std::string_view)) {
    std::conditional_t<IsMap, rt::OrderedMap<K, int>, rt::OrderedSet<K>> container;
    std::vector<std::string> out;
    const auto apply = [&](const std::string& op) {
      const auto tokens = split(op, ' ');
      switch (tokens[0][0]) {
        case 's':
          if constexpr (IsMap) container.set(parseKey(tokens[1]), std::stoi(tokens[2]));
          break;
        case 'a':
          if constexpr (!IsMap) container.add(parseKey(tokens[1]));
          break;
        case 'd':
          out.push_back(container.delete_(parseKey(tokens[1])) ? "d:true" : "d:false");
          break;
        case 'c':
          container.clear();
          break;
        case 'g':
          if constexpr (IsMap) {
            const int* value = container.get(parseKey(tokens[1]));
            out.push_back("g:" + (value ? show(*value) : "undefined"));
          }
          break;
        case 'h':
          out.push_back(container.has(parseKey(tokens[1])) ? "h:true" : "h:false");
          break;
        case 'z':
          out.push_back("z:" + std::to_string(container.size()));
          break;
        case 'k':
        case 'v': {
          std::string list;
          if constexpr (IsMap) {
            if (tokens[0][0] == 'k') {
              for (const auto& key : rt::arrayFrom(container.keys())) list += (list.empty() ? "" : ",") + show(key);
            } else {
              for (const auto& value : rt::arrayFrom(container.values())) list += (list.empty() ? "" : ",") + show(value);
            }
          } else {
            for (const auto& key : rt::arrayFrom(tokens[0][0] == 'k' ? container.keys() : container.values())) list += (list.empty() ? "" : ",") + show(key);
          }
          out.push_back(std::string(1, tokens[0][0]) + ":[" + list + "]");
          break;
        }
      }
    };
    for (const auto& op : split(script, ';')) {
      if (op[0] != 'i' && op[0] != 'f') {
        apply(op);
        continue;
      }
      std::map<int, std::vector<std::string>> actions;
      const auto parts = split(op, '@');
      for (std::size_t index = 1; index < parts.size(); ++index) {
        auto steps = split(parts[index], '/');
        const int visit = std::stoi(steps[0]);
        steps.erase(steps.begin());
        actions[visit] = steps;
      }
      int visits = 0;
      const auto visit = [&](const std::string& label) {
        out.push_back(label);
        if (const auto found = actions.find(visits); found != actions.end())
          for (const auto& action : found->second) apply(action);
        return ++visits >= kVisitCap;
      };
      if (op[0] == 'i') {
        for (const auto& entry : container) {
          std::string label;
          if constexpr (IsMap) label = show(entry.first) + "=" + show(entry.second);
          else label = show(entry);
          if (visit(label)) {
            out.push_back("cap");
            break;
          }
        }
      } else {
        try {
          if constexpr (IsMap) {
            container.forEach([&](int& value, const K& key) {
              if (visit("f" + show(key) + "=" + show(value))) {
                out.push_back("cap");
                throw Stop{};
              }
            });
          } else {
            container.forEach([&](const K& value) {
              if (visit("f" + show(value))) {
                out.push_back("cap");
                throw Stop{};
              }
            });
          }
        } catch (const Stop&) {
        }
      }
    }
    std::string trace;
    for (const auto& event : out) trace += (trace.empty() ? "" : "|") + event;
    return trace;
  }

  template <class K, bool IsMap, std::size_t N>
  void check(const Scenario (&scenarios)[N], K (*parseKey)(std::string_view)) {
    for (const auto& scenario : scenarios) {
      const std::string trace = run<K, IsMap>(scenario.script, parseKey);
      if (trace != scenario.trace) ::uos::rt::test::report(__FILE__, __LINE__, std::string(scenario.script) + "\n      actual:   " + trace + "\n      expected: " + std::string(scenario.trace), false);
    }
  }
}

TEST("Map with number keys matches JS Map, including mutation during iteration") { check<double, true>(kMapNumeric, numericKey); }
TEST("Set with number keys matches JS Set, including mutation during iteration") { check<double, false>(kSetNumeric, numericKey); }
TEST("Map with string keys matches JS Map") { check<std::string, true>(kMapString, stringKey); }
TEST("Set with string keys matches JS Set") { check<std::string, false>(kSetString, stringKey); }

TEST("string keys are found through std::string_view and const char*") {
  rt::OrderedMap<std::string, int> map{{"b", 1}, {"a", 2}, {"b", 3}};
  EXPECT_EQ(map.size(), 2u);
  EXPECT_EQ(*map.get("b"), 3);
  EXPECT_EQ(*map.get(std::string_view("a")), 2);
  EXPECT(map.get("c") == nullptr);
  EXPECT(map.has(std::string("a")));
  EXPECT(map.delete_(std::string_view("b")));
  EXPECT(!map.delete_("b"));
  EXPECT_EQ(rt::arrayFrom(map.keys()), std::vector<std::string>({"a"}));
  const auto entries = rt::arrayFrom(map);
  static_assert(std::is_same_v<decltype(entries), const std::vector<std::pair<std::string, int>>>);
  EXPECT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].first, "a");
  const std::vector<std::string> words{"q", "p", "q", "r", "p"};
  const rt::OrderedSet<std::string> unique(words.begin(), words.end());
  EXPECT_EQ(rt::arrayFrom(unique), std::vector<std::string>({"q", "p", "r"}));
  const std::vector<std::pair<std::string, int>> pairs{{"k", 1}, {"j", 2}, {"k", 3}};
  const rt::OrderedMap<std::string, int> fromPairs(pairs.begin(), pairs.end());
  const std::vector<std::pair<std::string, int>> expectedPairs{{"k", 3}, {"j", 2}};
  EXPECT_EQ(rt::arrayFrom(fromPairs), expectedPairs);
  rt::OrderedSet<std::string> set{"x", "y", "x"};
  EXPECT(set.has("y"));
  EXPECT_EQ(rt::arrayFrom(set), std::vector<std::string>({"x", "y"}));
}

TEST("entries stay readable after being deleted inside the loop that visits them") {
  rt::OrderedMap<std::string, std::string> map;
  for (int index = 0; index < 50; ++index) map.set("key" + std::to_string(index), "value" + std::to_string(index));
  int visited = 0;
  for (const auto& [key, value] : map) {
    map.delete_(key);
    for (int extra = 0; extra < 3; ++extra) map.delete_("key" + std::to_string(visited * 3 + extra + 1));
    EXPECT_EQ(value, "value" + key.substr(3));
    ++visited;
  }
  EXPECT_EQ(map.size(), 0u);
  EXPECT(visited > 0);
  map.set("again", "1");
  EXPECT_EQ(rt::arrayFrom(map.keys()), std::vector<std::string>({"again"}));
}

TEST("retired entries survive cursor copies and are reclaimed when the last cursor leaves") {
  rt::OrderedMap<int, std::shared_ptr<int>> map;
  auto value = std::make_shared<int>(42);
  const std::weak_ptr<int> retired = value;
  map.set(1, std::move(value));
  map.set(2, std::make_shared<int>(20));

  {
    auto first = map.begin();
    auto cursor = first;
    const auto& entry = *cursor;
    map.clear();
    EXPECT(!map.has(1));
    auto copy = map;
    EXPECT_EQ(copy.size(), 0u);

    for (int key = 1; key <= 128; ++key) map.set(key, std::make_shared<int>(key));
    first = map.end();
    ASSERT(!retired.expired());
    EXPECT_EQ(*entry.second, 42);
    ++cursor;
    ASSERT(cursor != map.end());
    EXPECT_EQ(cursor->first, 1);
    EXPECT_EQ(*cursor->second, 1);
  }
  EXPECT(retired.expired());
}

TEST("copies are independent and compacted; LRU eviction idiom") {
  rt::OrderedMap<int, int> map;
  for (int index = 0; index < 10; ++index) map.set(index, index * index);
  map.delete_(3);
  rt::OrderedMap<int, int> copy = map;
  copy.set(3, 9);
  map.set(0, -1);
  EXPECT_EQ(rt::arrayFrom(map.keys()), std::vector<int>({0, 1, 2, 4, 5, 6, 7, 8, 9}));
  EXPECT_EQ(rt::arrayFrom(copy.keys()), std::vector<int>({0, 1, 2, 4, 5, 6, 7, 8, 9, 3}));
  EXPECT_EQ(*copy.get(0), 0);
  // Refresh on access: delete + set moves the key to the end; evict the oldest key.
  const int value = *map.get(1);
  map.delete_(1);
  map.set(1, value);
  map.delete_(*map.keys().begin());
  EXPECT_EQ(rt::arrayFrom(map.keys()), std::vector<int>({2, 4, 5, 6, 7, 8, 9, 1}));
  copy = map;
  EXPECT_EQ(rt::arrayFrom(copy.values()), rt::arrayFrom(map.values()));
  const auto found = std::find_if(map.begin(), map.end(), [](const auto& entry) { return entry.second == 49; });
  ASSERT(found != map.end());
  EXPECT_EQ(found->first, 7);
  int sum = 0;
  map.forEach([&sum](int& entryValue, const int&) { sum += entryValue; });
  EXPECT_EQ(sum, 4 + 16 + 25 + 36 + 49 + 64 + 81 + 1);
}

TEST("heavy churn keeps insertion order and lookups exact") {
  std::mt19937 random(2026);
  rt::OrderedMap<int, int> map;
  std::vector<std::pair<int, int>> model;
  for (int step = 0; step < 20000; ++step) {
    const int key = static_cast<int>(random() % 64);
    const auto position = std::find_if(model.begin(), model.end(), [key](const auto& entry) { return entry.first == key; });
    switch (random() % 4) {
      case 0:
      case 1:
        map.set(key, step);
        if (position != model.end()) position->second = step;
        else model.emplace_back(key, step);
        break;
      case 2:
        EXPECT_EQ(map.delete_(key), position != model.end());
        if (position != model.end()) model.erase(position);
        break;
      default:
        if (random() % 200 == 0) {
          map.clear();
          model.clear();
        }
        break;
    }
    if (step % 97 == 0) {
      ASSERT_EQ(rt::arrayFrom(map), model);
      for (int probe = 0; probe < 64; ++probe) {
        const auto modelEntry = std::find_if(model.begin(), model.end(), [probe](const auto& entry) { return entry.first == probe; });
        const int* stored = map.get(probe);
        EXPECT_EQ(stored != nullptr, modelEntry != model.end());
        if (stored && modelEntry != model.end()) EXPECT_EQ(*stored, modelEntry->second);
      }
    }
  }
  EXPECT_EQ(map.size(), model.size());
}

TEST("SameValueZero keys: NaN finds NaN, -0 is stored as +0") {
  rt::OrderedSet<double> set{rt::Number::NaN, -0.0, 0.0, rt::Number::NaN};
  EXPECT_EQ(set.size(), 2u);
  EXPECT(set.has(rt::Number::NaN));
  const auto values = rt::arrayFrom(set);
  EXPECT(!std::signbit(values[1]));
  rt::OrderedMap<double, int> map;
  map.set(-0.0, 1);
  EXPECT_EQ(*map.get(0.0), 1);
  EXPECT(!std::signbit(map.begin()->first));
}
