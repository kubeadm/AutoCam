#ifndef RTSPSERVER_H
#define RTSPSERVER_H
#include "output.h"

#include <opencv2/core/mat.hpp>
#include <opencv2/highgui.hpp>

#include <iostream>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <chrono>
#include <thread>
#include "log.h"

using namespace cv;
using namespace std;
using namespace std::chrono;
using std::chrono::system_clock;
typedef cv::Point3_<unsigned char> Pixel;

class RtspServer : public Output
{
    string adr;
    string codec;
    int fps;
    int fdpipe;
    string fifoname;
    static int cnt;
    unsigned char *frameBuf;
    size_t buflen;
    bool firstFrameSent;
public:
    RtspServer(int _frameWidth, int _frameHeight, string _adr, string _codec, int _fps, int numOfchannels);

    ~RtspServer();
    void sendFrame(Mat &frame) const;
    void openPipe();

};

#endif // RTSPSERVER_H
