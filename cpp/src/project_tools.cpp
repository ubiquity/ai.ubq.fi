#include "pilot/core.hpp"
#include <algorithm>
#include <set>
namespace pilot {
  namespace {
    std::string unique(std::set<std::string>& used,std::string name) {
      auto r=name.substr(0,128);
      for(unsigned n=2;used.contains(r);++n) {
        auto suffix="_"+std::to_string(n);
        r=name.substr(0,128-suffix.size())+suffix;
      }
      used.insert(r);
      return r;
    } std::string mapped(Prepared const& r,std::string name,std::string const& space) {
      for(auto const& n:r.names)if(n.name==name&&(n.space?*n.space:std::string{})==space)return n.chat;
      return name;
    }
  } void project_tools(json::object const& raw,Prepared& r) {
    std::set<std::string> used;
    json::array tools(r.body.storage());
    auto add=[&](json::object const& o,std::shared_ptr<std::string const> space,bool custom) {
      auto name=field(o,"name");
      if(name.empty())throw Failure(400,"function tools require a name","invalid_request_error","tools.name");
      auto prefer=space&&used.contains(name)?*space+"_"+name:name;
      auto chat=unique(used,prefer);
      r.names.push_back( {
        chat,name,space,custom
      });
      json::object fn {
        {
          "name",chat
        }
      };
      if(member(o,"description").is_string())fn["description"]=member(o,"description");
      if(custom)fn["parameters"]=json::object {
        {
          "type","object"
        }, {
          "properties",json::object {
            {
              "input",json::object {
                {
                  "type","string"
                }, {
                  "description","Freeform input for the tool."
                }
              }
            }
          }
        }, {
          "required",json::array {
            "input"
          }
        }, {
          "additionalProperties",false
        }
      };
      else {
        if(!member(o,"parameters").is_null())fn["parameters"]=member(o,"parameters");
        if(member(o,"strict").is_bool())fn["strict"]=member(o,"strict");
      }
      tools.push_back(json::object {
        {
          "type","function"
        }, {
          "function",fn
        }
      });
      if(tools.size()>256)throw Failure(400,"Tool definition bound exceeded");
    };
    if(!member(raw,"tools").is_null()) {
      if(!member(raw,"tools").is_array())throw Failure(400,"tools must be an array","invalid_request_error","tools");
      for(auto const& v:member(raw,"tools").as_array()) {
        if(!v.is_object())throw Failure(400,"tools must contain objects");
        auto const& o=v.as_object();
        auto type=field(o,"type");
        if(type=="namespace") {
          auto namespace_owner=std::make_shared<std::string const>(field(o,"name"));
          auto const& nested=member(o,"tools");
          if(!nested.is_array())throw Failure(400,"namespace tools must nest a tools array");
          for(auto const& t:nested.as_array()) {
            if(!t.is_object())throw Failure(400,"namespace tools must contain objects");
            if(field(t.as_object(),"type")=="function")add(t.as_object(),namespace_owner,false);
          }
        }
        else if(type=="custom")add(o,nullptr,true);
        else if(member(o,"function").is_object())add(member(o,"function").as_object(),nullptr,false);
        else if(type=="function")add(o,nullptr,false);
      }
    } if(!tools.empty())r.body["tools"]=tools;
    auto const& choice=member(raw,"tool_choice");
    if(choice.is_string()) {
      auto v=text(choice);
      if(v!="none"&&v!="auto"&&v!="required")throw Failure(400,"Unsupported tool_choice","invalid_request_error","tool_choice");
      r.body["tool_choice"]=v;
    }
    else if(choice.is_object()) {
      auto const& o=choice.as_object();
      auto const& f=member(o,"function");
      auto name=field(o,"name",f.is_object()?field(f.as_object(),"name"):"");
      auto space=field(o,"namespace",f.is_object()?field(f.as_object(),"namespace"):"");
      if(name.empty())throw Failure(400,"a named tool_choice requires a name");
      r.body["tool_choice"]=json::object {
        {
          "type","function"
        }, {
          "function",json::object {
            {
              "name",mapped(r,name,space)
            }
          }
        }
      };
    }
    else if(!choice.is_null())throw Failure(400,"tool_choice must be a string or object");
    bool thinking=field(r.body,"reasoning_effort","high")!="none";
    if(thinking&&(field(r.body,"tool_choice")=="required"||member(r.body,"tool_choice").is_object()))throw Failure(400,"tool_choice is not supported while reasoning.effort keeps DeepSeek thinking mode active; set reasoning.effort to 'none' or use tool_choice 'auto'","invalid_request_error","tool_choice");
    if(!tools.empty()) {
      auto& messages=r.body["messages"].as_array();
      if(field(raw,"tool_choice")!="none") {
        std::string reminder="When tools are available, a progress update does not complete a requested action. If required work remains and you can perform it, continue with the next appropriate tool call instead of ending with a status message. Provide a final answer when the requested work is complete, or when you need user input or are blocked. Never claim a tool action has been done unless its result is in the conversation.";
        bool found=false;
        for(auto& v:messages)if(field(v.as_object(),"role")=="system"&&member(v.as_object(),"content").is_string()) {
          v.as_object()["content"]=bounded_payload(field(v.as_object(),"content")+"\n\n"+reminder,"instructions",field(raw,"truncation")=="disabled");
          found=true;
          break;
        }
        if(!found)messages.insert(messages.begin(),json::object {
          {
            "role","system"
          }, {
            "content",reminder
          }
        });
      }
      std::size_t tail=0;
      for(std::size_t i=0;i<messages.size();++i)if(field(messages[i].as_object(),"role")=="user")tail=i+1;
      for(std::size_t i=tail;i<messages.size();++i) {
        auto& m=messages[i].as_object();
        if(field(m,"role")=="assistant"&&!member(m,"reasoning_content").is_string())m["reasoning_content"]="";
      }
    }
  } Prepared project(json::object const& raw,Limits const& limits,json::storage_ptr sp) {
    static std::set<std::string> const allowed= {
      "background","context_management","conversation","include","input","instructions","max_output_tokens","max_tool_calls","metadata","model","moderation","parallel_tool_calls","previous_response_id","prompt","prompt_cache_key","prompt_cache_options","prompt_cache_retention","reasoning","safety_identifier","service_tier","store","stream","stream_options","temperature","text","tool_choice","tools","top_logprobs","top_p","truncation","user","client_metadata"
    };
    for(auto const& x:raw)if(!allowed.contains(std::string(x.key())))throw Failure(400,"Unrecognized request argument supplied: "+std::string(x.key()));
    Prepared r(sp);
    r.model=field(raw,"model");
    if(r.model!="deepseek-flash"&&r.model!="deepseek-v4-flash")throw Failure(400,"The requested model is not available","model_not_found","model");
    if(!member(raw,"stream").is_null()&&!member(raw,"stream").is_bool())throw Failure(400,"stream must be a boolean","invalid_request_error","stream");
    r.stream=flag(raw,"stream");
    if(!r.stream)throw Failure(400,"This pilot requires stream: true; buffered Responses are unsupported.","invalid_request_error","stream");
    auto trunc=field(raw,"truncation","auto");
    if(trunc!="auto"&&trunc!="disabled")throw Failure(400,"Unsupported truncation","invalid_request_error","truncation");
    for(auto key: {
      "max_output_tokens","max_tool_calls"
    })if(!member(raw,key).is_null()&&(integer(raw,key,-1)<=0))throw Failure(400,std::string(key)+" must be a positive integer","invalid_request_error",key);
    if(raw.contains("parallel_tool_calls")&&!member(raw,"parallel_tool_calls").is_bool())throw Failure(400,"parallel_tool_calls must be a boolean","invalid_request_error","parallel_tool_calls");
    if(!member(raw,"client_metadata").is_null()) {
      if(!member(raw,"client_metadata").is_object())throw Failure(400,"client_metadata must be an object with string values");
      for(auto const& x:member(raw,"client_metadata").as_object())if(!x.value().is_string())throw Failure(400,"client_metadata must contain string values");
    } if(!member(raw,"prompt_cache_options").is_null())throw Failure(400,"prompt_cache_options is not supported by this model","invalid_request_error","prompt_cache_options");
    json::array messages(sp);
    project_input(raw,messages,trunc=="disabled");
    if(messages.empty())throw Failure(400,"input must contain at least one message","invalid_request_error","input");
    r.body= {
      {
        "model","deepseek-flash"
      }, {
        "messages",messages
      }, {
        "stream",r.stream
      }
    };
    if(r.stream)r.body["stream_options"]=json::object {
      {
        "include_usage",true
      }
    };
    if(integer(raw,"max_output_tokens")>0)r.body["max_tokens"]=integer(raw,"max_output_tokens");
    if(!member(raw,"reasoning").is_null()) {
      if(!member(raw,"reasoning").is_object())throw Failure(400,"reasoning must be an object","invalid_request_error","reasoning");
      auto effort=field(member(raw,"reasoning").as_object(),"effort");
      if(!effort.empty())r.body["reasoning_effort"]=effort=="ultra"?"max":effort;
    } if(member(raw,"parallel_tool_calls").is_bool())r.body["parallel_tool_calls"]=member(raw,"parallel_tool_calls");
    bool thinking=field(r.body,"reasoning_effort","high")!="none";
    for(auto key: {
      "temperature","top_p"
    })if(raw.contains(key)) {
      if(thinking) {
        if(!r.warning.empty())r.warning+=", ";
        r.warning+=std::string(key)+"_ignored";
      }
      else r.body[key]=member(raw,key);
    } if(member(raw,"text").is_object()&&member(member(raw,"text").as_object(),"format").is_object()) {
      auto kind=field(member(member(raw,"text").as_object(),"format").as_object(),"type","text");
      if(kind=="json_object")r.body["response_format"]=json::object {
        {
          "type","json_object"
        }
      };
      else if(kind!="text")throw Failure(400,"text.format type '"+kind+"' is not supported upstream","invalid_request_error","text.format.type");
    } r.echo= {
      {
        "tools",member(raw,"tools").is_array()?member(raw,"tools"):json::value(json::array {
        })
      }, {
        "tool_choice",member(raw,"tool_choice").is_null()?json::value("auto"):member(raw,"tool_choice")
      }, {
        "parallel_tool_calls",flag(raw,"parallel_tool_calls",true)
      }, {
        "instructions",member(raw,"instructions").is_string()?member(raw,"instructions"):json::value(nullptr)
      }
    };

    if(json::serialize(r.echo).size()+1024>limits.terminal_bytes)throw Failure(400,"Terminal echo exceeds the pilot bound");
    project_tools(raw,r);
    auto wire=json::serialize(r.body);
    auto cost=estimate_tokens(wire)+256+r.body["messages"].as_array().size()*16+(member(r.body,"tools").is_array()?member(r.body,"tools").as_array().size()*32:0);
    auto reserve=std::max<std::int64_t>(16384,integer(r.body,"max_tokens"));
    if(reserve>=1000000||cost>static_cast<std::size_t>(1000000-reserve))throw Failure(400,"The complete projected DeepSeek prompt exceeds chat-prompt-admission/v3; no payload was dispatched.","context_length_exceeded","input");
    if(wire.size()>6*1024*1024||json::serialize(r.echo).size()+1024>limits.terminal_bytes)throw Failure(400,"Projected request or terminal echo exceeds the pilot bound");
    return r;
  }
}
