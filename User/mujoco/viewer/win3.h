#pragma once
#include <memory>
#include "platform_ui_adapter.h"

std::unique_ptr<mujoco::PlatformUIAdapter> MakeWin3Adapter();
void OpenWin3Model(mujoco::PlatformUIAdapter& adapter);
