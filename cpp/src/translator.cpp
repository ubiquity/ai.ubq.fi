#include "pilot/translator.hpp"
#include <algorithm>
#include <cmath>
namespace pilot {
  namespace {
    void append(json::array& target,json::array source) {
      for(auto& value:source)target.push_back(std::move(value));
    } json::object reasoning_item(std::string const& id,std::string const& value) {
      return {
        {
          "id",id
        }, {
          "type","reasoning"
        }, {
          "status","completed"
        }, {
          "summary",json::array {
            json::object {
              {
                "type","summary_text"
              }, {
                "text",value
              }
            }
          }
        }
      };
    } std::int64_t counter(json::object const& o,std::string_view key) {
      auto const& v=member(o,key);
      double n=v.is_int64()?static_cast<double>(v.as_int64()):v.is_uint64()?static_cast<double>(v.as_uint64()):v.is_double()?v.as_double():-1;
      return std::isfinite(n)&&n>=0&&n<=9007199254740991.0?static_cast<std::int64_t>(n):-1;
    }
  } Translator::Translator(Prepared const& r,Limits const& l,std::string id,json::storage_ptr sp):request_(r),limits_(l),storage_(sp),id_(std::move(id)),message_id_("msg_"+id_+"_0"),created_(now_ms()/1000),strings_(sp),calls_(sp),output_(sp),usage_(nullptr,sp) {
    strings_= {
      {
        "text",""
      }, {
        "refusal",""
      }, {
        "reasoning",""
      }
    };
  } json::object Translator::envelope(std::string const& status) {
    json::object r(request_.echo,storage_);
    r["id"]=id_;
    r["object"]="response";
    r["created_at"]=created_;
    r["status"]=status;
    r["model"]=request_.model;
    r["output"]=json::array {
    };
    r["error"]=nullptr;
    r["incomplete_details"]=nullptr;
    r["metadata"]=json::object {
    };
    for(auto k: {
      "temperature","top_p","max_output_tokens","previous_response_id","reasoning","usage"
    })r[k]=nullptr;
    r["store"]=false;
    r["truncation"]="disabled";
    return r;
  } json::array Translator::start() {
    json::array events(storage_);
    if(started_)return events;
    started_=true;
    auto r=envelope("in_progress");
    events.push_back(json::object {
      {
        "type","response.created"
      }, {
        "response",r
      }
    });
    events.push_back(json::object {
      {
        "type","response.in_progress"
      }, {
        "response",r
      }
    });
    return events;
  } json::array Translator::open() {
    return start();
  } void Translator::retain(std::string_view key,std::string_view delta) {
    auto& value=strings_[key].as_string();
    value.append(json::string_view(delta.data(),delta.size()));
    if(json::serialize(strings_).size()+json::serialize(calls_).size()+json::serialize(request_.echo).size()+1024>limits_.terminal_bytes)throw Failure(502,"Accumulated response exceeds the pilot terminal bound","deepseek_upstream_stream_error");
  } void Translator::open_reasoning(json::array& e) {
    if(reasoning_open_)return;
    reasoning_open_=true;
    reasoning_pending_=false;
    reasoning_index_=next_++;
    auto id="rs_"+id_+"_"+std::to_string(reasoning_index_);
    e.push_back(json::object {
      {
        "type","response.output_item.added"
      }, {
        "output_index",reasoning_index_
      }, {
        "item",json::object {
          {
            "id",id
          }, {
            "type","reasoning"
          }, {
            "status","in_progress"
          }, {
            "summary",json::array {
            }
          }
        }
      }
    });
    e.push_back(json::object {
      {
        "type","response.reasoning_summary_part.added"
      }, {
        "item_id",id
      }, {
        "output_index",reasoning_index_
      }, {
        "summary_index",0
      }, {
        "part",json::object {
          {
            "type","summary_text"
          }, {
            "text",""
          }
        }
      }
    });
  } void Translator::close_reasoning(json::array& e) {
    if(!reasoning_open_)return;
    reasoning_open_=false;
    auto id="rs_"+id_+"_"+std::to_string(reasoning_index_);
    auto value=field(strings_,"reasoning");
    auto item=reasoning_item(id,value);
    if(output_.size()<=static_cast<std::size_t>(reasoning_index_))output_.resize(static_cast<std::size_t>(reasoning_index_+1));
    output_[static_cast<std::size_t>(reasoning_index_)]=item;
    e.push_back(json::object {
      {
        "type","response.reasoning_summary_text.done"
      }, {
        "item_id",id
      }, {
        "output_index",reasoning_index_
      }, {
        "summary_index",0
      }, {
        "text",value
      }
    });
    e.push_back(json::object {
      {
        "type","response.reasoning_summary_part.done"
      }, {
        "item_id",id
      }, {
        "output_index",reasoning_index_
      }, {
        "summary_index",0
      }, {
        "part",json::object {
          {
            "type","summary_text"
          }, {
            "text",value
          }
        }
      }
    });
    e.push_back(json::object {
      {
        "type","response.output_item.done"
      }, {
        "output_index",reasoning_index_
      }, {
        "item",item
      }
    });
    strings_["reasoning"]="";
  } void Translator::open_message(json::array& e) {
    if(message_open_)return;
    close_reasoning(e);
    message_open_=true;
    message_index_=next_++;
    e.push_back(json::object {
      {
        "type","response.output_item.added"
      }, {
        "output_index",message_index_
      }, {
        "item",json::object {
          {
            "id",message_id_
          }, {
            "type","message"
          }, {
            "status","in_progress"
          }, {
            "role","assistant"
          }, {
            "content",json::array {
            }
          }
        }
      }
    });
  } void Translator::close_message(json::array& e) {
    if(!message_open_||message_done_)return;
    message_done_=true;
    json::array content(storage_);
    content.resize(static_cast<std::size_t>((text_index_>=0?1:0)+(refusal_index_>=0?1:0)));
    for(auto kind: {
      "text","refusal"
    }) {
      int index=std::string_view(kind)=="text"?text_index_:refusal_index_;
      if(index<0)continue;
      bool plain=std::string_view(kind)=="text";
      auto value=field(strings_,kind);
      json::object part=plain?json::object {
        {
          "type","output_text"
        }, {
          "text",value
        }, {
          "annotations",json::array {
          }
        }
      }
      :json::object {
        {
          "type","refusal"
        }, {
          "refusal",value
        }
      };
      json::object event {
        {
          "type",plain?"response.output_text.done":"response.refusal.done"
        }, {
          "item_id",message_id_
        }, {
          "output_index",message_index_
        }, {
          "content_index",index
        }
      };
      event[plain?"text":"refusal"]=value;
      if(plain)event["logprobs"]=json::array {
      };
      e.push_back(event);
      e.push_back(json::object {
        {
          "type","response.content_part.done"
        }, {
          "item_id",message_id_
        }, {
          "output_index",message_index_
        }, {
          "content_index",index
        }, {
          "part",part
        }
      });
      content[static_cast<std::size_t>(index)]=part;
    }
    json::object item {
      {
        "id",message_id_
      }, {
        "type","message"
      }, {
        "status","completed"
      }, {
        "role","assistant"
      }, {
        "content",content
      }
    };
    if(output_.size()<=static_cast<std::size_t>(message_index_))output_.resize(static_cast<std::size_t>(message_index_+1));
    output_[static_cast<std::size_t>(message_index_)]=item;
    e.push_back(json::object {
      {
        "type","response.output_item.done"
      }, {
        "output_index",message_index_
      }, {
        "item",item
      }
    });
  } void Translator::announce_tool(json::object& c,json::array& e) {
    close_reasoning(e);
    auto chat=field(c,"name");
    auto found=std::find_if(request_.names.begin(),request_.names.end(),[&](auto const& n) {
      return n.chat==chat;
    });
    bool custom=found!=request_.names.end()&&found->custom;
    auto id=std::string(custom?"ctc_":"fc_")+id_+"_"+field(c,"key");
    c["id"]=id;
    c["index"]=next_++;
    c["announced"]=true;
    c["custom"]=custom;
    c["original_name"]=found==request_.names.end()?chat:found->name;
    c["namespace"]=found==request_.names.end()||!found->space?"":*found->space;
    json::object item {
      {
        "id",id
      }, {
        "type",custom?"custom_tool_call":"function_call"
      }, {
        "status","in_progress"
      }, {
        "call_id",c["call_id"]
      }, {
        "name",c["original_name"]
      }
    };
    if(!field(c,"namespace").empty())item["namespace"]=c["namespace"];
    item[custom?"input":"arguments"]="";
    e.push_back(json::object {
      {
        "type","response.output_item.added"
      }, {
        "output_index",c["index"]
      }, {
        "item",item
      }
    });
  } json::array Translator::push(json::object const& chunk) {
    auto e=start();
    auto const& raw_usage=member(chunk,"usage");
    if(!raw_usage.is_null()) {
      if(!raw_usage.is_object())throw Failure(502,"Invalid upstream usage","deepseek_upstream_stream_error");
      auto const& u=raw_usage.as_object();
      auto input=counter(u,"prompt_tokens"),output=counter(u,"completion_tokens"),total=counter(u,"total_tokens");
      if(input<0||output<0||total<0)throw Failure(502,"Incomplete upstream usage","deepseek_upstream_stream_error");
      json::object usage {
        {
          "input_tokens",input
        }, {
          "output_tokens",output
        }, {
          "total_tokens",total
        }
      };
      auto cache=counter(u,"prompt_cache_hit_tokens");
      if(cache<0&&member(u,"prompt_tokens_details").is_object())cache=counter(member(u,"prompt_tokens_details").as_object(),"cached_tokens");
      if(cache>=0&&cache<=input)usage["input_tokens_details"]=json::object {
        {
          "cached_tokens",cache
        }
      };
      if(member(u,"completion_tokens_details").is_object()) {
        auto n=counter(member(u,"completion_tokens_details").as_object(),"reasoning_tokens");
        if(n>=0&&n<=output)usage["output_tokens_details"]=json::object {
          {
            "reasoning_tokens",n
          }
        };
      }
      usage_=usage;
    } auto const& choices=member(chunk,"choices");
    if(!choices.is_array())throw Failure(502,"Invalid upstream choices","deepseek_upstream_stream_error");
    for(auto const& v:choices.as_array()) {
      if(!v.is_object())throw Failure(502,"Invalid upstream choice");
      auto const& choice=v.as_object();
      if(member(choice,"finish_reason").is_string())finish_reason_=field(choice,"finish_reason");
      auto const& dv=member(choice,"delta");
      if(dv.is_null())continue;
      if(!dv.is_object())throw Failure(502,"Invalid upstream delta");
      auto const& d=dv.as_object();
      for(auto k: {
        "content","reasoning_content","refusal"
      })if(!member(d,k).is_null()&&!member(d,k).is_string())throw Failure(502,"Invalid upstream delta text");
      auto reason=field(d,"reasoning_content");
      if(!reason.empty()) {
        retain("reasoning",reason);
        if(reasoning_open_||next_==0) {
          open_reasoning(e);
          e.push_back(json::object {
            {
              "type","response.reasoning_summary_text.delta"
            }, {
              "item_id","rs_"+id_+"_"+std::to_string(reasoning_index_)
            }, {
              "output_index",reasoning_index_
            }, {
              "summary_index",0
            }, {
              "delta",reason
            }
          });
        }
        else reasoning_pending_=true;
      } for(auto kind: {
        "content","refusal"
      }) {
        auto value=field(d,kind);
        if(value.empty())continue;
        bool plain=std::string_view(kind)=="content";
        retain(plain?"text":"refusal",value);
        open_message(e);
        int& index=plain?text_index_:refusal_index_;
        if(index<0) {
          index=(text_index_>=0?1:0)+(refusal_index_>=0?1:0);
          json::object part=plain?json::object {
            {
              "type","output_text"
            }, {
              "text",""
            }, {
              "annotations",json::array {
              }
            }
          }
          :json::object {
            {
              "type","refusal"
            }, {
              "refusal",""
            }
          };
          e.push_back(json::object {
            {
              "type","response.content_part.added"
            }, {
              "item_id",message_id_
            }, {
              "output_index",message_index_
            }, {
              "content_index",index
            }, {
              "part",part
            }
          });
        }
        json::object event {
          {
            "type",plain?"response.output_text.delta":"response.refusal.delta"
          }, {
            "item_id",message_id_
          }, {
            "output_index",message_index_
          }, {
            "content_index",index
          }, {
            "delta",value
          }
        };
        if(plain)event["logprobs"]=json::array {
        };
        e.push_back(event);
      } auto const& tools=member(d,"tool_calls");
      if(!tools.is_null()&&!tools.is_array())throw Failure(502,"Invalid upstream tool calls");
      if(tools.is_array()) {
        std::size_t position=0;
        for(auto const& tv:tools.as_array()) {
          if(!tv.is_object())throw Failure(502,"Invalid upstream tool delta");
          auto const& t=tv.as_object();
          auto key=std::to_string(integer(t,"index",static_cast<std::int64_t>(position++)));
          if(!calls_.contains(key)) {
            if(calls_.size()>=256)throw Failure(502,"Tool call count exceeded");
            calls_[key]=json::object {
              {
                "key",key
              }, {
                "id",""
              }, {
                "call_id",field(t,"id",id_+"_call_"+key)
              }, {
                "name",""
              }, {
                "arguments",""
              }, {
                "announced",false
              }, {
                "index",-1
              }
            };
          }
          auto& c=calls_[key].as_object();
          if(!field(t,"id").empty())c["call_id"]=field(t,"id");
          auto const& fn=member(t,"function");
          if(fn.is_object()) {
            auto name=field(fn.as_object(),"name");
            if(!name.empty())c["name"]=name;
            auto args=field(fn.as_object(),"arguments");
            c["arguments"].as_string().append(args);
            if(!flag(c,"announced")&&!field(c,"name").empty()&&!reasoning_pending_)announce_tool(c,e);
            if(flag(c,"announced")&&!flag(c,"custom")&&!args.empty())e.push_back(json::object {
              {
                "type","response.function_call_arguments.delta"
              }, {
                "item_id",c["id"]
              }, {
                "output_index",c["index"]
              }, {
                "delta",args
              }
            });
          }
        }
      }
    }
    if(json::serialize(calls_).size()+json::serialize(strings_).size()+json::serialize(request_.echo).size()+1024>limits_.terminal_bytes)throw Failure(502,"Retained response exceeds bound");
    return e;
  } std::string Translator::terminal_kind() const {
    if(finish_reason_.empty()||finish_reason_=="stop"||finish_reason_=="tool_calls")return "response.completed";
    if(finish_reason_=="length"||finish_reason_=="content_filter")return "response.incomplete";
    return "response.failed";
  } json::array Translator::finish() {
    json::array e(storage_);
    if(finished_)return e;
    finished_=true;
    append(e,start());
    auto kind=terminal_kind();
    bool answer=!field(strings_,"text").empty()||!field(strings_,"refusal").empty();
    for(auto const& c:calls_)answer|=!field(c.value().as_object(),"name").empty();
    if(kind=="response.completed"&&!answer) {
      e.push_back(json::object {
        {
          "type","response.failed"
        }, {
          "response",json::object {
            {
              "id",id_
            }, {
              "object","response"
            }, {
              "status","failed"
            }, {
              "error",json::object {
                {
                  "code","empty_upstream_completion"
                }, {
                  "message","Upstream returned an empty completion."
                }
              }
            }
          }
        }
      });
      return e;
    }
    close_message(e);
    if(reasoning_pending_) {
      open_reasoning(e);
      e.push_back(json::object {
        {
          "type","response.reasoning_summary_text.delta"
        }, {
          "item_id","rs_"+id_+"_"+std::to_string(reasoning_index_)
        }, {
          "output_index",reasoning_index_
        }, {
          "summary_index",0
        }, {
          "delta",strings_["reasoning"]
        }
      });
      close_reasoning(e);
    }
    std::vector<int> keys;
    for(auto const& c:calls_)keys.push_back(std::stoi(std::string(c.key())));
    std::sort(keys.begin(),keys.end());
    for(int key:keys) {
      auto& c=calls_[std::to_string(key)].as_object();
      if(field(c,"name").empty())continue;
      if(!flag(c,"announced"))announce_tool(c,e);
      bool custom=flag(c,"custom");
      auto args=field(c,"arguments");
      json::object item {
        {
          "id",c["id"]
        }, {
          "type",custom?"custom_tool_call":"function_call"
        }, {
          "status",kind=="response.completed"?"completed":"incomplete"
        }, {
          "call_id",c["call_id"]
        }, {
          "name",c["original_name"]
        }
      };
      if(!field(c,"namespace").empty())item["namespace"]=c["namespace"];
      if(custom) {
        std::string input=args;
        try {
          auto v=json::parse(args);
          if(v.is_string())input=text(v);
          else if(v.is_object()&&member(v.as_object(),"input").is_string())input=field(v.as_object(),"input");
        }
        catch(std::exception const&) {
        }
        item["input"]=input;
        if(!input.empty())e.push_back(json::object {
          {
            "type","response.custom_tool_call_input.delta"
          }, {
            "item_id",c["id"]
          }, {
            "output_index",c["index"]
          }, {
            "call_id",c["call_id"]
          }, {
            "delta",input
          }
        });
      }
      else {
        item["arguments"]=args;
        e.push_back(json::object {
          {
            "type","response.function_call_arguments.done"
          }, {
            "item_id",c["id"]
          }, {
            "output_index",c["index"]
          }, {
            "arguments",args
          }
        });
      }
      auto index=static_cast<std::size_t>(integer(c,"index"));
      if(output_.size()<=index)output_.resize(index+1);
      output_[index]=item;
      e.push_back(json::object {
        {
          "type","response.output_item.done"
        }, {
          "output_index",c["index"]
        }, {
          "item",item
        }
      });
    }
    close_reasoning(e);
    auto response=envelope(kind=="response.completed"?"completed":kind=="response.incomplete"?"incomplete":"failed");
    response["output"]=output_;
    response["usage"]=usage_;
    if(kind=="response.incomplete")response["incomplete_details"]=json::object {
      {
        "reason",finish_reason_=="length"?"max_output_tokens":"content_filter"
      }
    };
    if(kind=="response.failed") {
      auto code=finish_reason_=="aborted"||finish_reason_=="insufficient_system_resource"?finish_reason_:"unrecognized_finish_reason:"+finish_reason_;
      response["error"]=json::object {
        {
          "code",code
        }, {
          "message","DeepSeek stopped generating: "+code
        }
      };
    }
    if(json::serialize(response).size()>limits_.terminal_bytes)throw Failure(502,"Terminal exceeds the pilot bound");
    e.push_back(json::object {
      {
        "type",kind
      }, {
        "response",response
      }
    });
    return e;
  } void SseReader::feed(std::string_view input) {
    pending_.append(input);
    if(pending_.size()>cap_)throw Failure(502,"Upstream SSE frame exceeded the gateway bound","deepseek_upstream_stream_error");
  } std::optional<std::string> SseReader::next() {
    std::size_t pos=std::string::npos,width=0;
    for(auto boundary: {
      "\r\n\r\n","\n\n","\r\r"
    }) {
      auto p=pending_.find(boundary);
      if(p<pos) {
        pos=p;
        width=std::char_traits<char>::length(boundary);
      }
    }
    if(pos==std::string::npos)return {
    };
    if(++frames_>max_frames_)throw Failure(502,"Upstream frame count exceeded");
    auto block=pending_.substr(0,pos);
    pending_.erase(0,pos+width);
    std::string data;
    bool any=false;
    std::size_t begin=0;
    while(begin<block.size()) {
      auto end=block.find_first_of("\r\n",begin);
      if(end==std::string::npos)end=block.size();
      auto line=block.substr(begin,end-begin);
      if(line=="data"||line.starts_with("data:")) {
        if(any)data+='\n';
        any=true;
        auto v=line=="data"?"":line.substr(5);
        if(!v.empty()&&v.front()==' ')v.erase(0,1);
        data+=v;
      }
      begin=end+1;
      if(end<block.size()&&block[end]=='\r'&&begin<block.size()&&block[begin]=='\n')++begin;
    }
    return data;
  }
}
