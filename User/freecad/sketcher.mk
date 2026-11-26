src := $(build)/FreeCAD-4fd3bf320d9566a27e60069fc8387448aaa3a094/src
gcs := $(src)/Mod/Sketcher/App/planegcs
objects := $(addprefix $(build)/obj/,GCS.o Constraints.o Geo.o SubSystem.o qp_eq.o)
includes := -I$(port)/sketcher-standalone -I$(src) -I$(gcs) -isystem $(build)/eigen/usr/include/eigen3 -isystem $(build)/boost/usr/include
flags := -Os -std=c++20 -DNDEBUG -D_GNU_SOURCE -include alloca.h -ffunction-sections -fdata-sections
$(build)/plane-gcs.a: $(objects)
	$(AR) rcs $@ $^
$(build)/obj/%.o: $(gcs)/%.cpp
	$(CXX) $(flags) $(includes) -MMD -MP -c $< -o $@
-include $(objects:.o=.d)
$(build)/sketch-probe: $(port)/sketch-probe.cpp $(build)/plane-gcs.a
	$(CXX) $(flags) $(includes) $^ -o $@
# The target runtime and the Alpine libstdc++ use different TLS models for std::future.
# Both QR analyses are independent and their results are consumed after the join.
$(build)/obj/GCS.cpp: $(gcs)/GCS.cpp $(port)/serial-qr.patch
	patch -s -o $@ $< < $(port)/serial-qr.patch
$(build)/obj/GCS.o: $(build)/obj/GCS.cpp
	$(CXX) $(flags) $(includes) -MMD -MP -c $< -o $@

part := $(src)/Mod/Part/App
occt := $(build)/occt-build
part_includes := -I$(port)/sketcher-standalone -I$(src) -I$(part) -I$(occt)/include/opencascade -include Standard_Version.hxx
part_objects := $(addprefix $(build)/obj/,FCBRepAlgoAPI_BooleanOperation.o FCBRepAlgoAPI_Cut.o FuzzyHelper.o part.o part-window.o)
$(build)/obj/%.o: $(part)/%.cpp
	$(CXX) $(flags) $(part_includes) -MMD -MP -c $< -o $@
$(build)/obj/part.o: $(port)/part.cpp
	$(CXX) $(flags) $(part_includes) -MMD -MP -c $< -o $@
$(build)/obj/part-window.o: $(port)/part-window.c
	$(CC) -Os -I$(port)/../tcc -MMD -MP -c $< -o $@
$(build)/part-window: $(part_objects)
	$(CXX) $^ -L$(occt)/lin64/gcc/lib -Wl,--start-group -lTKBO -lTKPrim -lTKShHealing -lTKTopAlgo -lTKGeomAlgo -lTKBRep -lTKGeomBase -lTKG3d -lTKG2d -lTKMath -lTKernel -Wl,--end-group -o $@
-include $(part_objects:.o=.d)
