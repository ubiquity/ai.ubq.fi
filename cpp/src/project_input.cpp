#include "pilot/core.hpp"
#include <algorithm>
namespace pilot {
  namespace {
    std::string reasoning(json::object const& item) {
      std::string result;
      for(auto key: {
        "summary","content"
      })if(auto parts=member(item,key).if_array())for(auto const& part:*parts)if(part.is_object())result+=field(part.as_object(),"text");
      return result+field(item,"text");
    } json::value content(json::value const& raw,std::string const& role,bool reject,std::string const& path,bool exempt,json::storage_ptr sp) {
      if(raw.is_string())return json::value(bounded_payload(text(raw),path+".content",reject));
      if(!raw.is_array())throw Failure(400,"input.content must be a string or an array","invalid_request_error","input.content");
      std::string joined;
      json::array images(sp);
      for(auto const& p:raw.as_array()) {
        if(!p.is_object())throw Failure(400,"input.content items must be objects");
        auto const& o=p.as_object();
        auto type=field(o,"type");
        if(type=="input_text"||type=="output_text"||type=="text") {
          if(!member(o,"text").is_string())throw Failure(400,"input.content text must be a string");
          joined+=field(o,"text");
        } else if(type=="refusal"&&role=="assistant") {
          if(!member(o,"refusal").is_string())throw Failure(400,"input.content refusal must be a string");
          joined+=field(o,"refusal");
        } else if(type=="input_image") {
          auto url=field(o,"image_url",field(o,"file_url"));
          if(url.empty())throw Failure(400,"input_image requires image_url");
          if(url.size()>65536) {
            if(reject)bounded_payload(url,path,reject);
            joined+="[gateway: deepseek-forwarded-payload/v2 image omitted ("+std::to_string(url.size())+" bytes); the model did not receive this image; this route forwards at most 65536 bytes per message because the provider counts forwarded payloads as text tokens]";
          }
          else {
            json::object image {
              {
                "url",url
              }
            };
            if(member(o,"detail").is_string())image["detail"]=member(o,"detail");
            images.push_back(json::object {
              {
                "type","image_url"
              }, {
                "image_url",std::move(image)
              }
            });
          }
        } else throw Failure(400,"input.content type '"+type+"' is not supported","invalid_request_error","input.content.type");
      } if(images.empty())return json::value(exempt?joined:bounded_payload(joined,path+".content",reject));
      if(!joined.empty())images.insert(images.begin(),json::object {
        {
          "type","text"
        }, {
          "text",joined
        }
      });
      std::size_t total=joined.size();
      for(auto const& p:images)if(field(p.as_object(),"type")=="image_url")total+=field(member(p.as_object(),"image_url").as_object(),"url").size();
      if(total>65536) {
        if(reject)throw Failure(400,"Image message exceeds forwarded payload bound","context_length_exceeded",path);
        return json::value(bounded_payload(json::serialize(images),path,reject));
      } return images;
    } bool summary_item(json::value const& raw,bool& seen) {
      if(raw.is_string()) {
        if(text(raw).find(summary_prefix)!=std::string::npos)throw Failure(400,"Summary prefix outside exact user input_text envelope","context_length_exceeded","input");
        return false;
      } if(!raw.is_object())return false;
      auto const& o=raw.as_object();
      auto const& c=member(o,"content");
      bool prefixed=false;
      if(c.is_string())prefixed=text(c).starts_with(summary_prefix);
      else if(c.is_array())for(auto const& p:c.as_array())if(p.is_object()&&field(p.as_object(),"text").starts_with(summary_prefix))prefixed=true;
      if(!prefixed)return false;
      bool valid=field(o,"role")=="user"&&field(o,"type","message")=="message"&&c.is_array()&&c.as_array().size()==1;
      if(valid) {
        auto const& p=c.as_array()[0];
        valid=p.is_object()&&field(p.as_object(),"type")=="input_text"&&field(p.as_object(),"text").starts_with(std::string(summary_prefix)+"\n");
      } if(!valid||seen)throw Failure(400,"Invalid or repeated codex-compaction-summary/v1 envelope","context_length_exceeded","input");
      seen=true;
      return true;
    }
  } void project_input(json::object const& raw,json::array& messages,bool reject) {
    auto sp=messages.storage();
    auto instructions=field(raw,"instructions");
    if(!instructions.empty())messages.push_back(json::object {
      {
        "role","system"
      }, {
        "content",bounded_payload(instructions,"instructions",reject)
      }
    });
    auto const& input=member(raw,"input");
    if(input.is_null())return;
    if(input.is_string()) {
      bool seen=false;
      summary_item(input,seen);
      if(!text(input).empty())messages.push_back(json::object {
        {
          "role","user"
        }, {
          "content",bounded_payload(text(input),"input",reject)
        }
      });
      return;
    } if(!input.is_array())throw Failure(400,"input must be a string or an array","invalid_request_error","input");
    if(input.as_array().size()>4096)throw Failure(400,"Input item bound exceeded");
    std::string pending;
    bool seen_summary=false;
    std::size_t index=0;
    for(auto const& item:input.as_array()) {
      auto path="input["+std::to_string(index++)+"]";
      bool exempt=summary_item(item,seen_summary);
      if(item.is_string()) {
        messages.push_back(json::object {
          {
            "role","user"
          }, {
            "content",bounded_payload(text(item),path,reject)
          }
        });
        pending.clear();
        continue;
      }
      if(!item.is_object())throw Failure(400,"input items must be objects");
      auto const& o=item.as_object();
      auto type=field(o,"type","message");
      if(type=="reasoning") {
        pending+=bounded_payload(reasoning(o),path,reject);
        continue;
      } if(type=="function_call"||type=="custom_tool_call") {
        auto id=field(o,"call_id",field(o,"id"));
        auto name=field(o,"name");
        if(id.empty()||name.empty())throw Failure(400,type+" items require call_id and name","invalid_request_error","input");
        auto args=type=="custom_tool_call"?json::serialize(json::object {
          {
            "input",field(o,"input")
          }
        }):field(o,"arguments","{}");
        json::object call {
          {
            "id",id
          }, {
            "type","function"
          }, {
            "function",json::object {
              {
                "name",name
              }, {
                "arguments",bounded_payload(args,path+".arguments",reject,true)
              }
            }
          }
        };
        if(!messages.empty()&&messages.back().is_object()&&field(messages.back().as_object(),"role")=="assistant"&&member(messages.back().as_object(),"content").is_null()&&member(messages.back().as_object(),"tool_calls").is_array()) {
          auto& last=messages.back().as_object();
          last["tool_calls"].as_array().push_back(call);
          if(!pending.empty())last["reasoning_content"]=field(last,"reasoning_content")+pending;
        } else {
          json::object turn {
            {
              "role","assistant"
            }, {
              "content",nullptr
            }, {
              "tool_calls",json::array {
                call
              }
            }
          };
          if(!pending.empty())turn["reasoning_content"]=pending;
          messages.push_back(turn);
        }
        pending.clear();
        continue;
      } if(type=="function_call_output"||type=="custom_tool_call_output") {
        auto id=field(o,"call_id");
        if(id.empty())continue;
        auto const& out=member(o,"output");
        auto value=out.is_string()?text(out):json::serialize(out.is_null()?json::value(""):out);
        messages.push_back(json::object {
          {
            "role","tool"
          }, {
            "tool_call_id",id
          }, {
            "content",bounded_payload(value,path+".output",reject)
          }
        });
        continue;
      } if(type=="agent_message") {
        std::string value="[agent message] "+field(o,"author","agent")+" -> "+field(o,"recipient","root")+":\n";
        std::size_t unreadable=0;
        if(auto parts=member(o,"content").if_array())for(auto const& part:*parts)if(part.is_object()) {
          auto t=field(part.as_object(),"text",field(part.as_object(),"encrypted_content"));
          if(t.starts_with("gAAAAA")&&t.size()>=100) {
            ++unreadable;
            continue;
          }
          if(value.back()!='\n')value+='\n';
          value+=t;
        }
        if(unreadable)value+="\n[gateway: "+std::to_string(unreadable)+" sealed agent-message part(s) were not readable and omitted]";
        messages.push_back(json::object {
          {
            "role","user"
          }, {
            "content",bounded_payload(value,path,reject)
          }
        });
        pending.clear();
        continue;
      } if(type!="message")throw Failure(400,"input item type '"+type+"' is not supported","invalid_request_error","input.type");
      auto role=field(o,"role","user");
      if(role!="user"&&role!="assistant"&&role!="developer"&&role!="system")throw Failure(400,"Unsupported input role","invalid_request_error","input.role");
      if(role=="user")pending.clear();
      json::object message {
        {
          "role",role=="developer"?"system":role
        }, {
          "content",content(member(o,"content"),role,reject,path,exempt,sp)
        }
      };
      if(role=="assistant"&&!pending.empty()) {
        message["reasoning_content"]=pending;
        pending.clear();
      }
      messages.push_back(message);
    }
  }
}
