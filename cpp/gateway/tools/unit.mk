# Driven by unit.sh. Objects are shared by every agent in this worktree: each is
# written to a private temporary name and renamed, so concurrent builds are safe.
OBJECTS := $(SOURCES:%.cpp=$(OUT)/obj/%.o)
VENDOR_OBJECTS := $(VENDORS:%=$(OUT)/vendor/%.o)
VENDOR_NAMES := $(sort $(foreach source,$(VENDORS),$(word 2,$(subst /, ,$(source)))))
VENDOR_LIBS := $(VENDOR_NAMES:%=$(OUT)/vendor/libuos_vendor_%.a)
VENDOR_FLAGS := $(filter-out -Wall -Wextra -Wshadow -Wconversion -Werror,$(CXXFLAGS))
VENDOR_C_FLAGS := $(filter-out -std=c++%, $(VENDOR_FLAGS)) -std=c11
QCBOR_INCLUDE := $(if $(filter qcbor,$(VENDOR_NAMES)),-isystem $(ROOT)/third_party/qcbor/inc)
ICU_LIBS := $(if $(filter src/rt/string.cpp,$(SOURCES)),-licui18n -licuuc)
.SECONDARY:
$(OUT)/obj/src/rt/regex_%.o: QUICKJS_INCLUDE := -isystem $(ROOT)/third_party/quickjs-libregexp
$(OUT)/vendor/third_party/quickjs-libregexp/%.c.o: VENDOR_C_FLAGS += -std=gnu11
$(OUT)/obj/%.o: $(ROOT)/%.cpp
	@mkdir -p $(@D)
	@echo "compile $*.cpp"
	@$(CXX_WRAPPER) $(CXXFLAGS) $(QCBOR_INCLUDE) $(QUICKJS_INCLUDE) $(if $(filter $*.cpp,$(SIMDJSON_SOURCES)),-DUOS_GATEWAY_SIMDJSON -isystem $(ROOT)/third_party/simdjson) -MMD -MP -MT $@ -MF $@.$(TOKEN).d -c $< -o $@.$(TOKEN).tmp && mv -f $@.$(TOKEN).d $(@:.o=.d) && mv -f $@.$(TOKEN).tmp $@
$(OUT)/vendor/%.cpp.o: $(ROOT)/%.cpp
	@mkdir -p $(@D)
	@$(CXX_WRAPPER) $(VENDOR_FLAGS) -MMD -MP -MT $@ -MF $@.$(TOKEN).d -c $< -o $@.$(TOKEN).tmp && mv -f $@.$(TOKEN).d $(@:.o=.d) && mv -f $@.$(TOKEN).tmp $@
$(OUT)/vendor/%.c.o: $(ROOT)/%.c
	@mkdir -p $(@D)
	@$(CXX_WRAPPER) --vendor-c $(VENDOR_C_FLAGS) $(if $(filter third_party/qcbor/%,$*),$(QCBOR_INCLUDE)) -MMD -MP -MT $@ -MF $@.$(TOKEN).d -c $< -o $@.$(TOKEN).tmp && mv -f $@.$(TOKEN).d $(@:.o=.d) && mv -f $@.$(TOKEN).tmp $@
define vendor_archive_rule
$(OUT)/vendor/libuos_vendor_$(1).a: $(filter $(OUT)/vendor/third_party/$(1)/%,$(VENDOR_OBJECTS)) $(ROOT)/tools/vendor_sources.py
	@$(CXX_WRAPPER) --vendor-archive rcs $$@.$(TOKEN).tmp $$(filter %.o,$$^) && mv -f $$@.$(TOKEN).tmp $$@
endef
$(foreach vendor,$(VENDOR_NAMES),$(eval $(call vendor_archive_rule,$(vendor))))
$(OUT)/obj/src/rt/json.o: $(ROOT)/cmake/vendors.json $(if $(SIMDJSON_SOURCES),$(ROOT)/third_party/simdjson/simdjson.h)
$(OUT)/bin/$(NAME): $(OBJECTS) $(VENDOR_LIBS) $(ROOT)/cmake/vendors.json
	@mkdir -p $(@D)
	@echo "link $(NAME)"
	@$(CXX_WRAPPER) -Wl,--no-keep-memory,--reduce-memory-overheads,--max-cache-size=67108864 $(OBJECTS) $(VENDOR_LIBS) $(LDLIBS) $(ICU_LIBS) -o $@.$(TOKEN).tmp && mv -f $@.$(TOKEN).tmp $@
-include $(OBJECTS:.o=.d) $(VENDOR_OBJECTS:.o=.d)
