#include "lane.hpp"

LaneDetect::LaneDetect()
{
    m_net = std::shared_ptr<MNN::Interpreter>(MNN::Interpreter::createFromFile(m_model_path));
    m_backend_config.precision = (MNN::BackendConfig::PrecisionMode) m_precision; // 精度
    m_backend_config.power = (MNN::BackendConfig::PowerMode) m_power; // 功耗
    m_backend_config.memory = (MNN::BackendConfig::MemoryMode) m_memory; // 内存占用
	m_config.backendConfig = &m_backend_config;
	m_config.type = MNN_FORWARD_AUTO;
    m_config.numThread = 4;
    
    MNN::CV::ImageProcess::Config img_config; // 图像处理
    ::memcpy(img_config.mean, m_mean_vals, sizeof(m_mean_vals)); // (img - mean)*norm
    ::memcpy(img_config.normal, m_norm_vals, sizeof(m_norm_vals));
    img_config.sourceFormat = MNN::CV::BGR;
    img_config.destFormat = MNN::CV::RGB;
    pretreat = std::shared_ptr<MNN::CV::ImageProcess>(MNN::CV::ImageProcess::create(img_config));
    MNN::CV::Matrix trans;
    trans.setScale(1.0f, 1.0f); // scale
    pretreat->setMatrix(trans);

	m_session = m_net->createSession(m_config); //创建session
    m_inTensor = m_net->getSessionInput(m_session, NULL);
    m_net->resizeTensor(m_inTensor, {1, 3, m_input_size, m_input_size});
    m_net->resizeSession(m_session);
	std::cout << "session created" << std::endl;
}


LaneDetect::~LaneDetect()
{
    m_net->releaseModel();
    m_net->releaseSession(m_session);
    m_lanes.clear();
}

inline int LaneDetect::clip(float value)
{
    if (value > 0 && value < m_input_size)
        return int(value);
    else if (value < 0)
        return 1;
    else
        return m_input_size - 1;

}

void LaneDetect::showImg(const cv::Mat& img,std::vector<LaneDetect::Lanes> Lanes)
{
    cv::Mat tmp_img = img.clone();
    int h = tmp_img.rows;
    int w = tmp_img.cols;
    
    for(auto line:Lanes)
    {
        float x1 = line.x1;
        float y1 = line.y1;
        float x2 = line.x2;
        float y2 = line.y2;
        x1 = clip(w * x1 / (m_input_size / 2));
        x2 = clip(w * x2 / (m_input_size / 2));
        y1 = clip(h * y1 / (m_input_size / 2));
        y2 = clip(h * y2 / (m_input_size / 2));

        cv::line(tmp_img,cv::Point(x1,y1),cv::Point(x2,y2), cv::Scalar(0, 0, 255));
    }

    cv::imshow("img",tmp_img);
    cv::waitKey(0);
    return ;
}



std::vector<LaneDetect::Lanes> LaneDetect::decodeHeatmap(const float* hm)
{   
    // 线段中心点(256*256),线段偏移(4*256*256)
    const float*  displacement = hm+m_hm_size*m_hm_size;
    // exp(center,center);
    std::vector<float> center;
    for (int i = 0;i < m_hm_size*m_hm_size; i++)
    {
        center.push_back( hm[i] ); // mlsd.mnn原始需要1/(exp(-hm[i]) + 1)
    }
    center.resize(m_hm_size*m_hm_size);

    std::vector<int> index(center.size(), 0);
    for (int i = 0 ; i != index.size() ; i++) {
        index[i] = i;
    }
    sort(index.begin(), index.end(),
        [&](const int& a, const int& b) {
            return (center[a] > center[b]); // 从大到小排序
        }
    );
    
    for (int i = 0; i < index.size(); i++)
    {
        int yy = index[i]/m_hm_size; // 除以宽得行号
        int xx = index[i]%m_hm_size; // 取余宽得列号
        Lanes Lane;
        Lane.x1 = xx + displacement[index[i] + 0*m_hm_size*m_hm_size];
        Lane.y1 = yy + displacement[index[i] + 1*m_hm_size*m_hm_size];
        Lane.x2 = xx + displacement[index[i] + 2*m_hm_size*m_hm_size];
        Lane.y2 = yy + displacement[index[i] + 3*m_hm_size*m_hm_size];
        Lane.lens = sqrt(pow(Lane.x1 - Lane.x2,2) + pow(Lane.y1 - Lane.y2,2));
        Lane.conf = center[index[i]];

        if (center[index[i]] > m_score_thresh && m_lanes.size() < m_top_k)
        {
            if ( Lane.lens > m_min_len)
                m_lanes.push_back(Lane);
        }
        else
            break;
    }
    
    return m_lanes;

}


const float* LaneDetect::inference(const cv::Mat& img)
{
    // 图像处理
    cv::Mat preImage = img.clone();
    cv::resize(preImage,preImage,cv::Size(m_input_size,m_input_size));
    pretreat->convert(preImage.data, m_input_size, m_input_size, 0, m_inTensor);
    // 推理
    m_net->runSession(m_session);
    MNN::Tensor *output= m_net->getSessionOutput(m_session, NULL);
    
    MNN::Tensor tensor_scores_host(output, output->getDimensionType());
    output->copyToHostTensor(&tensor_scores_host);

    auto score = output->host<float>(); // 得到结果指针
    return score;
}