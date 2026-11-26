#include <stdlib.h>
extern int __real_main(int argc,char **argv);
int __wrap_main(int argc,char **argv) {
    if(!getenv("RA2_INSTALL_DIR")) setenv("RA2_INSTALL_DIR","/lib/ra2-content",0);
    return __real_main(argc,argv);
}
