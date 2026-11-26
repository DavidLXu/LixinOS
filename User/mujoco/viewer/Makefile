dirs := $(SRC)/src/user $(SRC)/src/xml $(SRC)/src/xml/mjz $(SRC)/src/ui $(SRC)/src/render/classic $(SRC)/src/render/classic/glad $(SRC)/simulate $(DEPS)/qhull/src/libqhull_r $(DEPS)/tinyxml2 $(DEPS)/tinyobjloader $(DEPS)/miniz $(DEPS)/lodepng $(PORT)
empty :=
space := $(empty) $(empty)
VPATH := $(WORK)/adapt/simulate:$(WORK)/adapt/src/engine:$(WORK)/adapt/src/xml:$(WORK)/adapt/src/render/classic:$(subst $(space),:,$(dirs))
includes := $(SRC) $(PORT) $(PORT)/../../tcc $(SRC)/include $(SRC)/src $(SRC)/simulate $(DEPS)/qhull/src $(DEPS)/MarchingCubeCpp $(DEPS)/tinyxml2 $(DEPS)/tinyobjloader $(DEPS)/miniz $(DEPS)/lodepng $(GNOME)/sysroot/include $(WORK) $(PORT)/../../desktop
flags := -Os -ffunction-sections -fdata-sections -D_GNU_SOURCE -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES $(addprefix -I,$(includes))
sources := $(foreach dir,$(wordlist 1,6,$(dirs)),$(wildcard $(dir)/*.cc $(dir)/*.c)) $(wildcard $(DEPS)/qhull/src/libqhull_r/*.c $(DEPS)/miniz/miniz*.c) tinyxml2.cpp tiny_obj_loader.cc lodepng-export.cpp simulate.cc platform_ui_adapter.cc main.cc engine_thread.cc engine_util_errmem.c win3.cc transport.c export.c marching.cc
sources := $(filter-out %/loader.cc,$(sources))
objects := $(addprefix $(WORK)/obj/,$(addsuffix .o,$(basename $(notdir $(sources)))))
plugin_dirs := actuator sensor sdf obj_decoder stl_decoder gmsh_decoder
plugins := $(foreach dir,$(plugin_dirs),$(wildcard $(SRC)/plugin/$(dir)/*.cc))
objects += $(patsubst $(SRC)/plugin/%.cc,$(WORK)/obj/plugin/%.o,$(plugins))
$(WORK)/obj/plugin/%.o: $(SRC)/plugin/%.cc
	@mkdir -p $(dir $@)
	$(GNOME)/cxx $(flags) -std=gnu++20 -MMD -MP -c $< -o $@
$(WORK)/obj/engine_util_errmem.o: flags += -Dfopen=win3_export_open
$(WORK)/obj/%.o: %.cc
	$(GNOME)/cxx $(flags) -std=gnu++20 -MMD -MP -c $< -o $@
$(WORK)/obj/%.o: %.cpp
	$(GNOME)/cxx $(flags) -std=gnu++20 -MMD -MP -c $< -o $@
$(WORK)/obj/%.o: %.c
	$(GNOME)/cc $(flags) -std=gnu11 -MMD -MP -c $< -o $@
$(WORK)/simulate: $(objects) $(ENGINE) $(GNOME)/runtime/rt/liblx.a
	$(GNOME)/cxx -Wl,--gc-sections $(objects) -Wl,--start-group $(ENGINE) $(MESA_FLAGS) -lm -Wl,--end-group -o $@
-include $(objects:.o=.d)
