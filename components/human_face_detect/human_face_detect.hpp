#pragma once

#include "dl_detect_base.hpp"
#include "dl_detect_mnp_postprocessor.hpp"
#include "dl_detect_msr_postprocessor.hpp"
namespace human_face_detect {
class MSR : public dl::detect::DetectImpl {
public:
    MSR(const char *model_name);
};

class MNP {
private:
    dl::Model *m_model;
    dl::image::ImagePreprocessor *m_image_preprocessor;
    dl::detect::MNPPostprocessor *m_postprocessor;

public:
    MNP(const char *model_name);
    ~MNP();
    std::list<dl::detect::result_t> &run(const dl::image::img_t &img, std::list<dl::detect::result_t> &candidates);
    // 供两阶段检测器转发新版阈值配置和模型访问接口。
    void set_score_thr(float score_thr) { m_postprocessor->set_score_thr(score_thr); }
    void set_nms_thr(float nms_thr) { m_postprocessor->set_nms_thr(nms_thr); }
    dl::Model *get_raw_model() { return m_model; }
};

class MSRMNP : public dl::detect::Detect {
private:
    MSR *m_msr;
    MNP *m_mnp;

public:
    MSRMNP(const char *msr_model_name, const char *mnp_model_name) :
        m_msr(new MSR(msr_model_name)), m_mnp(new MNP(mnp_model_name)) {};
    ~MSRMNP();
    std::list<dl::detect::result_t> &run(const dl::image::img_t &img) override;
    // idx 为阶段索引：0 表示 MSR，1 表示 MNP。
    Detect &set_score_thr(float score_thr, int idx) override;
    Detect &set_nms_thr(float nms_thr, int idx) override;
    dl::Model *get_raw_model(int idx) override;
};

} // namespace human_face_detect

class HumanFaceDetect : public dl::detect::DetectWrapper {
public:
    // 检测模型类型。
    typedef enum { MSRMNP_S8_V1 } model_type_t;
    HumanFaceDetect(const char *sdcard_model_dir = nullptr,
                    model_type_t model_type = static_cast<model_type_t>(CONFIG_HUMAN_FACE_DETECT_MODEL_TYPE));

private:
    std::string m_sdcard_model_dir; // SD 卡模型目录，持有副本以保证生命周期。
    model_type_t m_model_type; // 当前选择的模型。
    void load_model() override; // 加载原有检测模型。
};
