// Recorded from src/models/codex-models-availability.ts under Bun 1.4.0 (scratch script rec-availability.ts):
// JSON.stringify of each result.
const char* const AVAILABILITY_RECORDED = R"JSON({
 "parse": {
  "orders index-like ids first and trims": {
   "input": "{\"accounts\":{\"2\":{\"client_version\":\"v\",\"slugs\":[\"s\"],\"updated_at_ms\":1},\"10\":{\"client_version\":\"v\",\"slugs\":[\"s\"],\"updated_at_ms\":1},\"b\":{\"client_version\":\"v\",\"slugs\":[\"s\"],\"updated_at_ms\":1},\" \":{\"client_version\":\"v\",\"slugs\":[\"s\"],\"updated_at_ms\":1},\"x\":{\"client_version\":\" 0.1 \",\"slugs\":[\" a \",\"a\",5,\"\",\"b\",null],\"updated_at_ms\":1}},\"unsupported\":{\"z\":{\"2\":3,\"10\":4,\"m\":1,\" \":2,\"n\":1.5,\"o\":-1,\"p\":9007199254740992},\"e\":{},\"a\":[7,8]}}",
   "output": "{\"accounts\":{\"2\":{\"client_version\":\"v\",\"slugs\":[\"s\"],\"updated_at_ms\":1},\"10\":{\"client_version\":\"v\",\"slugs\":[\"s\"],\"updated_at_ms\":1},\"b\":{\"client_version\":\"v\",\"slugs\":[\"s\"],\"updated_at_ms\":1},\"x\":{\"client_version\":\"0.1\",\"slugs\":[\"a\",\"b\"],\"updated_at_ms\":1}},\"unsupported\":{\"z\":{\"2\":3,\"10\":4,\"m\":1},\"a\":{\"0\":7,\"1\":8}}}"
  },
  "array accounts and null unsupported": {
   "input": "{\"accounts\":[{\"client_version\":\"v\",\"slugs\":[],\"updated_at_ms\":1}],\"unsupported\":null}",
   "output": "{\"accounts\":{\"0\":{\"client_version\":\"v\",\"slugs\":[],\"updated_at_ms\":1}},\"unsupported\":{}}"
  },
  "array store": {
   "input": "[]",
   "output": "null"
  },
  "slugs that are not an array": {
   "input": "{\"accounts\":{\"a\":{\"client_version\":\"v\",\"slugs\":\"x\",\"updated_at_ms\":1}}}",
   "output": "{\"accounts\":{},\"unsupported\":{}}"
  },
  "non-positive and unsafe timestamps": {
   "input": "{\"accounts\":{\"a\":{\"client_version\":\"v\",\"slugs\":[],\"updated_at_ms\":0},\"b\":{\"client_version\":\"v\",\"slugs\":[],\"updated_at_ms\":-5},\"c\":{\"client_version\":\"v\",\"slugs\":[],\"updated_at_ms\":1.5},\"d\":{\"client_version\":\"v\",\"slugs\":[],\"updated_at_ms\":9007199254740991}}}",
   "output": "{\"accounts\":{\"d\":{\"client_version\":\"v\",\"slugs\":[],\"updated_at_ms\":9007199254740991}},\"unsupported\":{}}"
  },
  "proto key": {
   "input": "{\"accounts\":{\"__proto__\":{\"client_version\":\"v\",\"slugs\":[\"s\"],\"updated_at_ms\":1},\"a\":{\"client_version\":\"v\",\"slugs\":[\"s\"],\"updated_at_ms\":1}},\"unsupported\":{\"__proto__\":{\"m\":1},\"b\":{\"__proto__\":2,\"m\":3}}}",
   "output": "{\"accounts\":{\"a\":{\"client_version\":\"v\",\"slugs\":[\"s\"],\"updated_at_ms\":1}},\"unsupported\":{\"b\":{\"m\":3}}}"
  },
  "only unsupported": {
   "input": "{\"unsupported\":{\"a\":{\"m\":5}},\"extra\":1}",
   "output": "{\"accounts\":{},\"unsupported\":{\"a\":{\"m\":5}}}"
  },
  "blank client version": {
   "input": "{\"accounts\":{\"a\":{\"client_version\":\"   \",\"slugs\":[\"s\"],\"updated_at_ms\":1}},\"unsupported\":{}}",
   "output": "{\"accounts\":{},\"unsupported\":{}}"
  }
 },
 "parseBytes": "{\"accounts\":{},\"unsupported\":{\"k\":{\"0\":5,\"2\":6},\"j\":{\"1\":300}}}",
 "with": {
  "ties sort by localeCompare after index keys": "{\"accounts\":{\"a\":{\"client_version\":\"v\",\"slugs\":[\"m0\"],\"updated_at_ms\":1}},\"unsupported\":{\"a\":{\"10\":7,\"b\":5,\"B\":5,\"x\":5,\"y\":5,\"z\":5},\"c\":{\"old\":1}}}",
  "NaN observation is pruned": "{\"accounts\":{\"a\":{\"client_version\":\"v\",\"slugs\":[\"m0\"],\"updated_at_ms\":1}},\"unsupported\":{\"a\":{\"10\":7,\"b\":5,\"B\":5,\"x\":5,\"y\":5},\"c\":{\"old\":1}}}",
  "aged entries are pruned": "{\"accounts\":{\"a\":{\"client_version\":\"v\",\"slugs\":[\"m0\"],\"updated_at_ms\":1}},\"unsupported\":{\"a\":{\"10\":7,\"x\":5,\"y\":5,\"B\":5,\"b\":5},\"c\":{\"new\":604800002}}}",
  "proto account id is an own key": "{\"accounts\":{\"a\":{\"client_version\":\"v\",\"slugs\":[\"m0\"],\"updated_at_ms\":1}},\"unsupported\":{\"a\":{\"10\":7,\"x\":5,\"y\":5,\"B\":5,\"b\":5},\"c\":{\"old\":1},\"__proto__\":{\"m\":5}}}",
  "infinite observation is kept": "{\"accounts\":{\"a\":{\"client_version\":\"v\",\"slugs\":[\"m0\"],\"updated_at_ms\":1}},\"unsupported\":{\"a\":{\"10\":7,\"inf\":null,\"b\":5,\"B\":5,\"x\":5,\"y\":5},\"c\":{\"old\":1}}}",
  "existing model moves nowhere": "{\"accounts\":{\"a\":{\"client_version\":\"v\",\"slugs\":[\"m0\"],\"updated_at_ms\":1}},\"unsupported\":{\"a\":{\"10\":7,\"y\":8,\"b\":5,\"B\":5,\"x\":5},\"c\":{\"old\":1}}}"
 },
 "merge": {
  "clears advertised models and keeps the rest": "{\"accounts\":{\"a\":{\"client_version\":\"v2\",\"slugs\":[\"m1\",\"m3\"],\"updated_at_ms\":100},\"c\":{\"client_version\":\"v\",\"slugs\":[],\"updated_at_ms\":9}},\"unsupported\":{\"a\":{\"m2\":6},\"b\":{\"m1\":7}}}",
  "an account whose models all clear leaves unsupported": "{\"accounts\":{\"a\":{\"client_version\":\"v\",\"slugs\":[\"m0\"],\"updated_at_ms\":1},\"b\":{\"client_version\":\"v\",\"slugs\":[\"m1\"],\"updated_at_ms\":100}},\"unsupported\":{\"a\":{\"m1\":5,\"m2\":6}}}",
  "no entries returns the store": "{\"accounts\":{\"a\":{\"client_version\":\"v\",\"slugs\":[\"m0\"],\"updated_at_ms\":1}},\"unsupported\":{\"a\":{\"m1\":5,\"m2\":6},\"b\":{\"m1\":7}}}",
  "proto account id is not set": "{\"accounts\":{\"a\":{\"client_version\":\"v\",\"slugs\":[\"m0\"],\"updated_at_ms\":1}},\"unsupported\":{\"a\":{\"m1\":5,\"m2\":6},\"b\":{\"m1\":7}}}",
  "index-like account ids": "{\"accounts\":{\"7\":{\"client_version\":\"v\",\"slugs\":[],\"updated_at_ms\":3},\"z\":{\"client_version\":\"v\",\"slugs\":[],\"updated_at_ms\":3},\"07\":{\"client_version\":\"v\",\"slugs\":[],\"updated_at_ms\":3}},\"unsupported\":{}}"
 }
})JSON";
