#include <iostream>
#include "facelock/lbph_wrapper.h"
#include <opencv2/opencv.hpp>

int main(int argc, char** argv){
    if(argc<3){ std::cerr<<"usage: test_predict <user> <image>\n"; return 2; }
    std::string user = argv[1];
    cv::Mat im = cv::imread(argv[2], cv::IMREAD_GRAYSCALE);
    if(im.empty()){ std::cerr<<"failed to load image\n"; return 3; }
    facelock::LBPHWrapper w;
    if(!w.load(user,"data/models")) { std::cerr<<"failed to load model\n"; return 4; }
    auto res = w.predict(im, 0.30f);
    if(!res) { std::cerr<<"predict failed\n"; return 5; }
    std::cout<<"match="<<res->match<<" score="<<res->score<<" sample="<<res->best_sample<<"\n";
    return 0;
}
