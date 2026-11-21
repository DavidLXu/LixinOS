# Added after upstream's link flags so the static archives can resolve cycles.
LDFLAGS += $(PORT_OBJECTS) $(PORT_LIBS) -Wl,--end-group
$(EXETARGET): $(PORT_OBJECTS)
