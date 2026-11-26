#include <Inventor/SoDB.h>
#include <Inventor/actions/SoGLRenderAction.h>
#include <Inventor/nodes/SoBaseColor.h>
#include <Inventor/nodes/SoCube.h>
#include <Inventor/nodes/SoLightModel.h>
#include <Inventor/nodes/SoOrthographicCamera.h>
#include <Inventor/nodes/SoSeparator.h>
#include <GL/gl.h>
#include <cstdio>

bool test_scene()
{
    SoDB::init();
    auto *scene = new SoSeparator;
    scene->ref();
    auto *camera = new SoOrthographicCamera;
    camera->position.setValue(0, 0, 5);
    camera->height = 5;
    scene->addChild(camera);
    auto *lightModel = new SoLightModel;
    lightModel->model = SoLightModel::BASE_COLOR;
    scene->addChild(lightModel);
    auto *color = new SoBaseColor;
    color->rgb.setValue(0, 1, 0);
    scene->addChild(color);
    scene->addChild(new SoCube);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    {
        SoGLRenderAction render(SbViewportRegion(32, 32));
        render.apply(scene);
    }
    unsigned char center[4] = {}, corner[4] = {};
    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
    bool passed = center[1] == 255 && center[2] == 0 && corner[1] == 0 && corner[2] == 255;
    std::printf("Coin %s cube readback %s (center %u,%u,%u; corner %u,%u,%u)\n",
                SoDB::getVersion(), passed ? "passed" : "failed", center[0], center[1], center[2],
                corner[0], corner[1], corner[2]);
    scene->unref();
    return passed;
}
