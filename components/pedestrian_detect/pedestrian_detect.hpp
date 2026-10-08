#pragma once

#include "dl_detect_base.hpp"
#include "dl_detect_pico_postprocessor.hpp"

namespace pedestrian_detect {
class Pico : public dl::detect::DetectImpl {
public:
    Pico(const char *model_name);
};
} // namespace pedestrian_detect

class PedestrianDetect : public dl::detect::DetectWrapper {
public:
    // 检测模型类型。
    typedef enum { PICO_S8_V1 } model_type_t;
    PedestrianDetect(const char *sdcard_model_dir = nullptr,
                     model_type_t model_type = static_cast<model_type_t>(CONFIG_PEDESTRIAN_DETECT_MODEL_TYPE));

private:
    std::string m_sdcard_model_dir; // SD 卡模型目录，持有副本以保证生命周期。
    model_type_t m_model_type; // 当前选择的模型。
    void load_model() override; // 加载原有检测模型。
};
