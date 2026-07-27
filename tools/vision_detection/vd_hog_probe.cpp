#include <opencv2/opencv.hpp>
#include <cstdio>
int main(){
  cv::HOGDescriptor h(cv::Size(64,64),cv::Size(16,16),cv::Size(8,8),cv::Size(8,8),9);
  cv::Mat c(64,64,CV_8UC3), g(64,64,CV_8UC1);
  cv::randu(c,0,255); cv::cvtColor(c,g,cv::COLOR_BGR2GRAY);
  std::vector<float> dc,dg; h.compute(c,dc); h.compute(g,dg);
  double diff=0; for(size_t i=0;i<dc.size();i++) diff+=fabs(dc[i]-dg[i]);
  printf("color_dim=%zu gray_dim=%zu L1diff=%.3f\n",dc.size(),dg.size(),diff);
  return 0;
}
