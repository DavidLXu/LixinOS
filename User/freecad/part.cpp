// Use FreeCAD's Part boolean operation and OCCT's native BRep exchange format.
#include "part.h"
#include <FCBRepAlgoAPI_Cut.h>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS.hxx>
#include <TopExp_Explorer.hxx>
#include <Standard_Failure.hxx>
#include <fcntl.h>
#include <unistd.h>
#include <sstream>
#include <fstream>

static TopoDS_Shape model;

int part_plate(double radius)
{
    if (!(radius > 0 && radius < 10)) return 0;
    try {
        auto stock = BRepPrimAPI_MakeBox(36, 24, 8).Shape();
        gp_Ax2 axis(gp_Pnt(18, 12, -1), gp_Dir(0, 0, 1));
        auto drill = BRepPrimAPI_MakeCylinder(axis, radius, 10).Shape();
        FCBRepAlgoAPI_Cut cut(stock, drill);
        if (!cut.IsDone() || cut.Shape().IsNull()) return 0;
        model = cut.Shape();
        return 1;
    } catch (const Standard_Failure&) { return 0; }
}

double part_volume(void)
{
    GProp_GProps props;
    BRepGProp::VolumeProperties(model, props);
    return props.Mass();
}

int part_save(const char *path)
{
    std::ostringstream stream;
    BRepTools::Write(model, stream);
    if (!stream) return 0;
    auto data = stream.str();
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) return 0;
    size_t offset = 0;
    while (offset < data.size()) {
        ssize_t n = write(fd, data.data() + offset, data.size() - offset);
        if (n <= 0) break;
        offset += n;
    }
    bool closed = close(fd) == 0;
    if (offset == data.size() && closed) return 1;
    unlink(path);
    return 0;
}

int part_read(const char *path)
{
    try {
        std::ifstream input(path);
        if (!input) return 0;
        TopoDS_Shape next;
        BRep_Builder builder;
        BRepTools::Read(next, input, builder);
        if (next.IsNull() || !input) return 0;
        model = next;
        return 1;
    } catch (const Standard_Failure&) { return 0; }
}

void part_edges(Line3D line)
{
    for (TopExp_Explorer it(model, TopAbs_EDGE); it.More(); it.Next()) {
        BRepAdaptor_Curve curve(TopoDS::Edge(it.Current()));
        double start = curve.FirstParameter(), end = curve.LastParameter();
        int steps = curve.GetType() == GeomAbs_Line ? 1 : 48;
        gp_Pnt prev = curve.Value(start);
        for (int i = 1; i <= steps; ++i) {
            gp_Pnt p = curve.Value(start + (end - start) * i / steps);
            line(prev.X(), prev.Y(), prev.Z(), p.X(), p.Y(), p.Z());
            prev = p;
        }
    }
}
