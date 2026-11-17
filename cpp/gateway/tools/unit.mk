# Driven by unit.sh. Objects are shared by every agent in this worktree: each is
# written to a private temporary name and renamed, so concurrent builds are safe.
OBJECTS := $(SOURCES:%.cpp=$(OUT)/obj/%.o)
.SECONDARY:
$(OUT)/obj/%.o: $(ROOT)/%.cpp
	@mkdir -p $(@D)
	@echo "compile $*.cpp"
	@$(CXX_WRAPPER) $(CXXFLAGS) -MMD -MP -MT $@ -MF $@.$(TOKEN).d -c $< -o $@.$(TOKEN).tmp && mv -f $@.$(TOKEN).d $(@:.o=.d) && mv -f $@.$(TOKEN).tmp $@
$(OUT)/bin/$(NAME): $(OBJECTS)
	@mkdir -p $(@D)
	@echo "link $(NAME)"
	@$(CXX_WRAPPER) -Wl,--no-keep-memory,--reduce-memory-overheads,--max-cache-size=67108864 $(OBJECTS) $(LDLIBS) -o $@.$(TOKEN).tmp && mv -f $@.$(TOKEN).tmp $@
-include $(OBJECTS:.o=.d)
