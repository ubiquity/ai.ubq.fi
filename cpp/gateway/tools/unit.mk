# Driven by unit.sh. Objects are shared by every agent in this worktree: each is
# written to a private temporary name and renamed, so concurrent builds are safe.
OBJECTS := $(SOURCES:%.cpp=$(OUT)/obj/%.o)
VENDOR_OBJECTS := $(VENDORS:%=$(OUT)/vendor/%.o)
VENDOR_LIBS := $(VENDORS:%=$(OUT)/vendor/%.a)
VENDOR_FLAGS := $(filter-out -Wall -Wextra -Wshadow -Wconversion -Werror,$(CXXFLAGS))
.SECONDARY:
$(OUT)/obj/%.o: $(ROOT)/%.cpp
	@mkdir -p $(@D)
	@echo "compile $*.cpp"
	@$(CXX_WRAPPER) $(CXXFLAGS) $(if $(filter $*.cpp,$(SIMDJSON_SOURCES)),-DUOS_GATEWAY_SIMDJSON -isystem $(ROOT)/third_party/simdjson) -MMD -MP -MT $@ -MF $@.$(TOKEN).d -c $< -o $@.$(TOKEN).tmp && mv -f $@.$(TOKEN).d $(@:.o=.d) && mv -f $@.$(TOKEN).tmp $@
$(OUT)/vendor/%.cpp.o: $(ROOT)/%.cpp
	@mkdir -p $(@D)
	@$(CXX_WRAPPER) $(VENDOR_FLAGS) -MMD -MP -MT $@ -MF $@.$(TOKEN).d -c $< -o $@.$(TOKEN).tmp && mv -f $@.$(TOKEN).d $(@:.o=.d) && mv -f $@.$(TOKEN).tmp $@
$(OUT)/vendor/%.a: $(OUT)/vendor/%.o
	@$(CXX_WRAPPER) --vendor-archive rcs $@.$(TOKEN).tmp $< && mv -f $@.$(TOKEN).tmp $@
$(OUT)/obj/src/rt/json.o: $(ROOT)/cmake/vendors.json $(if $(SIMDJSON_SOURCES),$(ROOT)/third_party/simdjson/simdjson.h)
$(OUT)/bin/$(NAME): $(OBJECTS) $(VENDOR_LIBS) $(ROOT)/cmake/vendors.json
	@mkdir -p $(@D)
	@echo "link $(NAME)"
	@$(CXX_WRAPPER) -Wl,--no-keep-memory,--reduce-memory-overheads,--max-cache-size=67108864 $(OBJECTS) $(VENDOR_LIBS) $(LDLIBS) -o $@.$(TOKEN).tmp && mv -f $@.$(TOKEN).tmp $@
-include $(OBJECTS:.o=.d) $(VENDOR_OBJECTS:.o=.d)
