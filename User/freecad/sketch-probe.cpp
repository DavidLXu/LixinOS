// Exercise the upstream Sketcher solver independently of the Qt application.
#include "GCS.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: freecad-sketch-probe BASE LENGTH\n");
        return 2;
    }
    char *end;
    double base = std::strtod(argv[1], &end);
    if (*end || !(base > 0) || !std::isfinite(base)) return 2;
    double length = std::strtod(argv[2], &end);
    if (*end || !(length > base / 2) || !std::isfinite(length)) return 2;

    // Two fixed endpoints and two distance constraints locate the upper vertex.
    double zero = 0, x = base * 0.4, y = length * 0.8;
    GCS::Point a(&zero, &zero), b(&base, &zero), c(&x, &y);
    GCS::VEC_pD unknowns{&x, &y};
    GCS::System sketch;
    sketch.addConstraintP2PDistance(a, c, &length, 1);
    sketch.addConstraintP2PDistance(b, c, &length, 2);
    sketch.declareUnknowns(unknowns);
    sketch.initSolution();
    if (sketch.solve() != GCS::Success) return 1;
    sketch.applySolution();
    if (std::abs(std::hypot(x, y) - length) > 1e-6 ||
        std::abs(std::hypot(x - base, y) - length) > 1e-6 || y <= 0) return 1;
    std::printf("PlaneGCS triangle (0, 0) (%.6f, 0) (%.6f, %.6f) mm\n", base, x, y);
    return 0;
}
