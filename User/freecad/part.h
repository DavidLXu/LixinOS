#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int part_plate(double radius);
double part_volume(void);
int part_save(const char *path);
int part_read(const char *path);
typedef void (*Line3D)(double, double, double, double, double, double);
void part_edges(Line3D line);
#ifdef __cplusplus
}
#endif
