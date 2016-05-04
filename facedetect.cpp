#include "opencv2/objdetect.hpp"
#include "opencv2/highgui.hpp"
#include "opencv2/imgproc.hpp"
#include <iostream>
#include <iomanip>
#include <string>
#include <sstream>
#include <fstream>
#include <cstdlib>
#include <opencv2/videoio/videoio.hpp>  // Video write
#include <ctime>

using namespace std;
using namespace cv;
const double FI=1.61803398;
typedef enum {STOP,START,MOVE,END} DYNAMIC_STATES;
static void help()
{
    cout << "Build date:" << __DATE__ << " " << __TIME__
            "During execution:\n\tHit any key to quit.\n"
            "\tUsing OpenCV version " << CV_VERSION << "\n" << endl;
}

void detectAndDraw( Mat& img, CascadeClassifier& cascade,
                    CascadeClassifier& nestedCascade,
                    double scale, bool tryflip );

string cascadeFullName = "../../data/haarcascades/haarcascade_frontalface_alt.xml";
string cascadeProfName = "../../data/haarcascades/haarcascade_profileface.xml";
string cascadeLEyeName = "../../data/haarcascades/haarcascade_lefteye_2splits.xml";
string cascadeREyeName = "../../data/haarcascades/haarcascade_righteye_2splits.xml";

inline Point rectCenterAbs(const Rect2f& r){ // absolute coordinates
    int w=r.width;
    return Point(r.x+(int)(w*0.5), r.y+(int)(r.height*0.5));
}
inline Point topMiddleDec(const Rect2f& r) {return Point(cvRound((double)r.width*0.5) , cvRound((double)r.height/3.0));} // relative coordinates
inline Point topLeftDec(const Rect2f& r) {return Point(cvRound((double)r.width/3.0) , cvRound((double)r.height/3.0));} // relative coordinates
inline Point topRightDec(const Rect2f& r){return Point(cvRound((double)r.width*2.0/3.0) , cvRound((double)r.height/3.0));} // relative coordinates
Point getGoldenPoint(const Rect2f& roi,const Rect& face){ // absolute coordinates
    Point target;
    if(cvRound((float)roi.width/3.0) - face.width < 0 ) /// если лицо крупное, то держать его в центре кадра
        target = topMiddleDec(roi);
    else if(face.x+cvRound((float)face.width/2.0) < topMiddleDec(roi).x
            && face.x < roi.x+topLeftDec(roi).x)
        target = topLeftDec(roi);
    else if(face.x+face.width > roi.x+topRightDec(roi).x) // Камера посередине не будет реагировать
        target = topRightDec(roi);
    else
        target = topMiddleDec(roi);

    Point result = (face+topMiddleDec(face) - target).tl();/// Должна быть зависимость только от размеров ROI
    return result;
}
/// PID - регулятор для позиционирования камеры
class PIDController{
    double errPrev;
    double Kp, Ki, Kd;//0.001 , 0.05
    double stor;
    double u;
    double maxU;
    double minU;
public:
    PIDController(const double& kp,const double& ki,const double& kd, const double& max_u, const double& min_u=0){
        errPrev=stor=0.0;
        Kp=kp, Ki=ki, Kd=kd;//0.001 , 0.05
        minU=min_u;
        maxU=max_u;
    }
    double getU(const double& err){
        stor   += err;
        u     = err*Kp + stor*Ki + (err-errPrev)*Kd;
        errPrev = err;

        if(u>maxU)u=maxU;
        else if(u<-maxU)u=-maxU;
        else if(0<u && u<minU)u=minU;
        else if(-minU<u && u<0)u=-minU;

        return u;
    }
};
int gcd(int a,int b){
    int c;
    while (a != 0){
        c = a;
        a = b%a;
        b = c;
    }
    return b;
}
Size getAspect(Size sz){
    int g=gcd(sz.width,sz.height);
    return Size(sz.width/g,sz.height/g);
}

inline void scaleRect(Rect2f &r,const Size& asp, const float &sc=1.0){ /// from center
    r.height+=2*asp.height*sc;
    r.width+=2*asp.width*sc;
    r.x -= asp.width*sc;
    r.y -= asp.height*sc;
}

void autoZoom(const Rect& face,
              Rect2f& roi,
              const Size& aspect,const float& maxStep, const double& relation){
    static float step=0.01;
    if(maxStep>step)step+=0.01;else step -= 0.01;
       if(roi.height > face.height*relation)
           scaleRect(roi,aspect,-step);
       else
           scaleRect(roi,aspect,step);
}

void autoMove(const Rect& face,
                     Rect2f& roi,
                     const int& maxStepX, const int& maxStepY)
{
    static PIDController pidX(0.1, 0.001, -0.09,maxStepX);
    static PIDController pidY(0.1, 0.001, -0.09,maxStepY);

    Point p(getGoldenPoint(roi,face));
    roi.x += pidX.getU(p.x-roi.x);
    roi.y += pidY.getU(p.y-roi.y);
}
class autoMotion{
     DYNAMIC_STATES state;
     double speedMin;
     double speedMax;
     double speedInc;
     double accelTime;
     double speed;
     float sign;
public:
    autoMotion(double spdMin,double spdMax){
        state = STOP;
        speedMin=spdMin;
        speedMax=spdMax;
        speed=spdMin;
    }
    float update(float& x,const int& aim, const double& precision){
        accelTime = precision*2.0/speedMax;
        speedInc=(speedMax-speedMin)/accelTime;
        switch (state) {
        case STOP:
            if(abs(aim-cvRound(x))>2*precision){
                if(aim>cvRound(x)) sign=1.0; else sign=-1.0;
                state = START;
            }
            break;
        case START:
            speed+=speedInc;
            x += sign*speed;
            if(speed>speedMax) {state=MOVE;speed=speedMax;}
            break;
        case MOVE:
            x += sign*speed;
            if(abs(aim-cvRound(x))<precision) state=END;
            break;
        case END:
            speed-=speedInc;
            x += sign*speed;
            if(speed<speedMin) {state=STOP; speed=speedMin;}
            break;
        }
        return speed;
    }
    DYNAMIC_STATES getState(){
         return state;
    }
    float getSign(){ return sign;}
    double getSpeed(){
        return speed;
    }
};

class FaceFilter
{
    Rect facePrev;
    bool hasPrevFace;
    int area;
public:
    FaceFilter()
        : hasPrevFace(false){}
    int isNear(const Rect faceCur){ // returns true if previous face detection was nearby the current
        if(hasPrevFace) {
            if(isNear(faceCur,facePrev)){
                facePrev = faceCur;
                return true;
            }else return false;
        }
        else{
            hasPrevFace=false;
            facePrev=faceCur;
            return true;
        }
    }
    static int isNear(const Rect face1, const Rect face2 ){ // returns true if previous face detection was nearby the current
        return (face1 & face2).area();
    }
};
class FaceFilterArray{
    Rect facePrev;
    bool hasPrevFace;
public:
    FaceFilterArray() {
    }
    void filter(vector<Rect>& faces){
        if(hasPrevFace)
            for (int i = 0; i < faces.size(); ++i) {
                if((facePrev & faces[i]).area()<=0)faces.erase(faces.begin()+i);
            }
    }
    inline void setFacePrev(const Rect fp){
        facePrev=fp;
        hasPrevFace=true;
    }
};

void drawRects(Mat& img, const vector<Rect>& rects,
               string t="rect", Scalar color=Scalar(255,0,0),
               float fontScale=1.0,
               float textThickness=1.0,
               int textOffset=0,
               int thickness=1,
               int fontFace=CV_FONT_NORMAL){
    for (int i = 0; i < rects.size(); ++i)
    {
        stringstream title;
        title << t<<" "<< i;
        putText(img, title.str(),
                Point(rects[i].x,
                      rects[i].y-textOffset),
                fontFace, fontScale,color,textThickness);
        rectangle(img,rects[i],
                  color, thickness, 8, 0);
    }
}

inline void drawGoldenRules(Mat& img, const Rect2f& r,Scalar color=Scalar(0,255,0),const double& dotsRadius=1){
    //Отрисовка точек золотого сечения
    circle(img,Point(r.x + r.width/3.0,
                              r.y + r.height/3.0), 1,Scalar(0,255,0), dotsRadius);
    circle(img,Point(r.x + 2.0*r.width/3.0,
                              r.y + r.height/3.0),1,Scalar(0,255,0), dotsRadius);
    circle(img,Point(r.x + r.width/3.0,
                              r.y + 2.0*r.height/3.0),1,Scalar(0,255,0),dotsRadius);
    circle(img,Point(r.x + 2.0*r.width/3.0,
                              r.y + 2.0*r.height/3.0),1,Scalar(0,255,0),dotsRadius);
}



int detectMotion(Mat img, int thresh=50, int blur=21, bool showPrev=false){
    static Mat diff,gr, grLast;
    int motion=0;
    cvtColor( img, gr, COLOR_BGR2GRAY );
    if (!grLast.empty())
    {
        GaussianBlur(gr, gr, Size(blur,blur), 0,0);
        absdiff(gr, grLast, diff);
        threshold(diff, diff, thresh, 255, cv::THRESH_BINARY);
        motion=(int)mean(diff)[0]; // Движение в кадрике обнаружено

        if(showPrev){
            stringstream motion;
            motion<<"Motion "<<(int)mean(diff)[0];
            resize( diff, diff, Size(240*diff.cols/diff.rows,240), 0, 0, INTER_NEAREST );
            putText(diff, motion.str(),
                    Point(0,diff.rows),CV_FONT_NORMAL, 0.5, Scalar(255, 0,0));
            imshow("diff",diff);
        }
    }
    grLast=gr.clone();
    return motion;
}
Rect median(const vector<Rect>& r){
    static vector<int> x,y,h;
    x.resize(r.size());
    y.resize(r.size());
    h.resize(r.size());
    for (int i = 0; i < r.size(); ++i) {
        x[i]=r[i].x;
        y[i]=r[i].y;
        h[i]=r[i].height;
    }
    sort(x.begin(),x.end());
    sort(y.begin(),y.end());
    sort(h.begin(),h.end());
    return Rect(x[x.size()/2],y[y.size()/2],h[h.size()/2],h[h.size()/2]);
}

int main( int argc, const char** argv )
{
    VideoCapture capture;
    Mat frame, image;

    const string scaleOpt = "--scale=";
    size_t scaleOptLen = scaleOpt.length();
    const string cascadeOpt = "--cascade=";
    size_t cascadeOptLen = cascadeOpt.length();
    const string nestedCascadeOpt = "--nested-cascade";
    size_t nestedCascadeOptLen = nestedCascadeOpt.length();
    const string tryFlipOpt = "--try-flip";
    const string showPrevOpt =  "--show-preview";
    const string recPrevOpt = "--record-preview";
    const string roiSizeOpt = "--roiSize";
    size_t tryFlipOptLen = tryFlipOpt.length();
    string inputName;
    bool tryflip = false;
    bool showPreview = false;
    bool recordPreview = false;
    help();

    CascadeClassifier cascadeFull,cascadeProf, cascadeEyeL,cascadeEyeR; // Cascades for Full face and Profile face
    double scale = 1;

    for( int i = 1; i < argc; i++ )
    {
        cout << "Processing " << i << " " <<  argv[i] << endl;
        if( cascadeOpt.compare( 0, cascadeOptLen, argv[i], cascadeOptLen ) == 0 )
        {
            cascadeFullName.assign( argv[i] + cascadeOptLen );
            cout << "  from which we have cascadeName= " << cascadeFullName << endl;
        }
        else if( nestedCascadeOpt.compare( 0, nestedCascadeOptLen, argv[i], nestedCascadeOptLen ) == 0 )
        {
            cout << "nc" <<endl;
            if( argv[i][nestedCascadeOpt.length()] == '=' )
                cascadeLEyeName.assign( argv[i] + nestedCascadeOpt.length() + 1 );

        }
        else if( scaleOpt.compare( 0, scaleOptLen, argv[i], scaleOptLen ) == 0 )
        {
            if( !sscanf( argv[i] + scaleOpt.length(), "%lf", &scale ) || scale < 1 )
                scale = 1;
            cout << " from which we read scale = " << scale << endl;
        }
        else if( tryFlipOpt.compare( 0, tryFlipOptLen, argv[i], tryFlipOptLen ) == 0 )
        {
            tryflip = true;
            cout << " will try to flip image horizontally to detect assymetric objects\n";
        }
        else if( argv[i][0] == '-' )
        {
            cerr << "WARNING: UnkoneIterEndn option %s" << argv[i] << endl;
        }
        else
            inputName.assign( argv[i] );
        if( string::npos!=showPrevOpt.find(argv[i]))
        {
            showPreview = true;
        }
        if( string::npos!=recPrevOpt.find(argv[i]))
        {
            recordPreview = true;
        }
    }

    if( !cascadeEyeL.load( cascadeLEyeName ) )
        cerr << "WARNING: Could not load classifier cascade for nested objects" << endl;
    if( !cascadeEyeR.load( cascadeREyeName ) )
        cerr << "WARNING: Could not load classifier cascade for nested objects" << endl;
    if( !cascadeFull.load( cascadeFullName ) )
    {
        cerr << "ERROR: Could not load classifier cascade" << endl;
        help();
        return -1;
    }

    if( !cascadeProf.load( cascadeProfName ) )
    {
        cerr << "ERROR: Could not load classifier 2 cascade" << endl;
        help();
        return -1;
    }
    bool isWebcam=false;
    if( inputName.empty() || (isdigit(inputName.c_str()[0]) && inputName.c_str()[1] == '\0') )
    {
        isWebcam=true;
        int c = inputName.empty() ? 0 : inputName.c_str()[0] - '0' ;
        if(!capture.open(c))
            cout << "Capture from camera #" <<  c << " didn't work" << endl;
    }
    else if( inputName.size() )
    {
        isWebcam=false;
        image = imread( inputName, 1 );
        if( image.empty() )
        {
            if(!capture.open( inputName ))
                cout << "Could not read " << inputName << endl;
        }
    }
    else
    {
        image = imread( "../data/lena.jpg", 1 );
        if(image.empty()) cout << "Couldn't read ../data/lena.jpg" << endl;
    }
    if( capture.isOpened() )
    {
        cout << "Video capturing has been started ..." << endl;

   //    MODEL    //
        // frames
        Mat fullFrame;
        Mat smallImg;
        Mat graySmall;
        Mat result;

        /// Face detection
        const double fx = 1 / scale;
        const int minNeighbors=1; // количество соседних лиц // \todo 26\04\2016 Вводить из командной строки
        const double scaleFactor=1.25; // \todo 26\04\2016 Вводить из командной строки
        const Size minfaceSize=Size(25,25); // \todo 26\04\2016 Вводить из командной строки

        FaceFilterArray filt;

        bool foundFaces=false;
        vector<Rect> facesFull,facesProf,faceBuf;

        //Video characteristics
        const long int videoLength = capture.get(CAP_PROP_FRAME_COUNT);
        const float aspectRatio = (float)capture.get(CV_CAP_PROP_FRAME_WIDTH)/
                                  (float)capture.get(CV_CAP_PROP_FRAME_HEIGHT);
        const Rect2f fullShot(0,0,(int)capture.get(CV_CAP_PROP_FRAME_WIDTH),
                                (int)capture.get(CV_CAP_PROP_FRAME_HEIGHT));
        int fps;
        int fourcc;
        long int frameCounter=0;


        const Size smallImgSize = Size((float)fullShot.width/scale, (float)fullShot.height/scale);
        const Size maxRoiSize = smallImgSize;
        const Size previewSize = smallImgSize;
        const Size resultSize = Size(720*aspectRatio,720); // \todo 26\04\2016 Вводить из командной строки

        ///Zoom & movement params (driver)
        const double onePerc =(double)smallImgSize.width/100.0; // onePercent
        float maxStepX = (0.9); // \todo 26\04\2016 Вводить из командной строки
        float maxStepY = (0.9);// \todo 26\04\2016 Вводить из командной строки
        float maxScaleSpeed = 0.3;// \todo 26\04\2016 Вводить из командной строки
        Point gp;
        cout << " maxStepX:"<< maxStepX << " maxStepY:"<< maxStepY << " maxStepZ:"<< maxScaleSpeed << endl;
        //zooming
        const int stopZoomThr = cvRound(10.0*onePerc); // \todo 26\04\2016 Вводить из командной строки
        const float zoomThr=FI; // \todo 26\04\2016 Вводить из командной строки
        const double face2shot = FI; // \todo 26\04\2016 Вводить из командной строки
        const unsigned int aimUpdateFreq=9; // каждые Н кадров // \todo 26\04\2016 Вводить из командной строки
        const unsigned int faceDetectFreq=1; // \todo 26\04\2016 Вводить из командной строки
        const Size aspect = getAspect(fullShot.size());

        Rect aim=Rect(Point(0,0),maxRoiSize);
        Rect2f roi = Rect2f(Point(0,0),maxRoiSize);

        const bool bZoom = true;
        const bool bMove = true;
        double minZoomSpeed=0.01, // \todo 26\04\2016 Вводить из командной строки
                maxZoomSpeed=0.2, // \todo 26\04\2016 Вводить из командной строки
                zoomSpeedInc=(maxZoomSpeed-minZoomSpeed)/10.0, // \todo 26\04\2016 Вводить из командной строки
                zoomSpeed=minZoomSpeed;
        DYNAMIC_STATES zoomState = STOP;
        autoMotion moveX(0,maxStepX*onePerc),moveY(0,maxStepY*onePerc);
        double zoomSign = 1;

        //file writing
        stringstream outFileTitle;
        VideoWriter previewVideo;
        VideoWriter outputVideo;

        ///Test items
        const double ticksPerMsec=cvGetTickFrequency() * 1.0e3;
        vector<int64> tmr;
        vector<int> lines;
        fstream logFile;

   //    VIEW    //
        Mat preview;

        //drawing
        const float thickness = 0.5*previewSize.width/100.0;
        const int dotsRadius = thickness*2;
        const int textOffset = thickness*2;
        const int textThickness = thickness/2.0;
        const double fontScale = thickness/5;
        string prevWindTitle = "Preview";
        cout << "Aspect:" << aspect << endl;
/// SetUp
        if(isWebcam){
            time_t t = time(0);   // get time now
            struct tm * now = localtime( & t );
             outFileTitle << "webcam"
                          << (now->tm_year + 1900) << '_'
                          << (now->tm_mon + 1) << '_'
                          << now->tm_mday << "_"
                          << now->tm_hour <<"-"
                          << now->tm_min << "_"
                          << __DATE__ <<" "<< __TIME__ <<"_sc"<< scale;
        }else{
            outFileTitle << inputName.substr(inputName.find_last_of('/')+1)
            << __DATE__ <<" "<< __TIME__<<"_sc"<< scale;
        }

        if(isWebcam){
            fps = capture.get(CAP_PROP_FPS)/5.0;
            fourcc = VideoWriter::fourcc('M','J','P','G'); // codecs
        }
        else{
            fourcc = capture.get(CV_CAP_PROP_FOURCC); // codecs
            fps = capture.get(CAP_PROP_FPS);
        }
        if(!outputVideo.open("results/closeUp_"+outFileTitle.str()+".avi",fourcc,
                                         fps, resultSize, true)){
            cout << "Could not open the output video ("
                 << "results/closeUp_"+outFileTitle.str()+".avi" <<") for writing"<<endl;
            return -1;
        }
        if(recordPreview){
             if(!previewVideo.open("results/test_"+outFileTitle.str()+".avi",fourcc,
                                   fps, previewSize, true))
             {
                 cout << "Could not open the output video ("
                      << "results/test_"+outFileTitle.str()+".avi" <<") for writing"<<endl;
                 return -1;
             }
        }
        logFile.open(("results/test_"+outFileTitle.str()+".csv").c_str(), fstream::out);
        if(!logFile.is_open()){
            cout << "Error with opening the file:" << "results/test_"+outFileTitle.str()+".csv" << endl;
        }
        //// Main cycle
        for(;;)
        {

            tmr.push_back(cvGetTickCount());if(frameCounter<2)lines.push_back(__LINE__);
            try{
                capture >> fullFrame;
                if(fullFrame.empty()) {
                    clog << "Frame is empty" << endl;
                    break;
                }


                tmr.push_back(cvGetTickCount());if(frameCounter<2)lines.push_back(__LINE__);
                if(frameCounter%faceDetectFreq==0){
                    resize( fullFrame, smallImg, smallImgSize, 0, 0, INTER_LINEAR );
                    cvtColor( smallImg, graySmall, COLOR_BGR2GRAY );
                /// \todo 04/05/2016 Искать лица только в некоторой области вокруг уже обнаруженного лица

                    /* Поиск лиц в анфас */
                    cascadeFull.detectMultiScale(graySmall, facesFull,
                                                 scaleFactor, minNeighbors, 0|CASCADE_SCALE_IMAGE,minfaceSize);
                    /// Поиск лиц в профиль
                    cascadeProf.detectMultiScale( graySmall, facesProf,
                                                  scaleFactor, minNeighbors, 0|CASCADE_SCALE_IMAGE,minfaceSize);
                    tmr.push_back(cvGetTickCount());if(frameCounter<2)lines.push_back(__LINE__);
                    foundFaces = !(facesFull.empty() && facesProf.empty());
                }else foundFaces = false;
                tmr.push_back(cvGetTickCount());if(frameCounter<2)lines.push_back(__LINE__);

                    // \todo 26\04\2016 Создать фильтр для прямоугольников лиц

                if(foundFaces){
                    if(!facesFull.empty() && ((facesFull[0]&aim).area()>0))faceBuf.push_back(facesFull[0]);
                    if(!facesProf.empty() && ((facesProf[0]&aim).area()>0))faceBuf.push_back(facesProf[0]);
                }
                if(frameCounter%aimUpdateFreq == 0){
                    if(!faceBuf.empty()) {
                        aim = median(faceBuf);
                        faceBuf.clear();
                    }
                }
            }catch(Exception &mvEx){
                cout << "Detection block: "<< mvEx.msg << endl;
            }
            /// Motion and zoom
            tmr.push_back(cvGetTickCount());if(frameCounter<2)lines.push_back(__LINE__);
            try{
                if(bZoom){
                float aimH = aim.height*face2shot;
                switch (zoomState) {
                case STOP:
                    if(aimH>roi.height*zoomThr){
                        zoomSign=1;
                        zoomState=START;
                    }
                    if(aimH<roi.height/zoomThr){
                        zoomSign=-1;
                        zoomState=START;
                    }
                    break;
                case START:
                    zoomSpeed+=zoomSpeedInc;
                    scaleRect(roi,aspect,zoomSign*zoomSpeed);
                    if(zoomSpeed > maxZoomSpeed) {zoomState=MOVE;zoomSpeed=maxZoomSpeed;}
                    if(roi.height > maxRoiSize.height) {
                        roi.height=maxRoiSize.height;
                        roi.width=maxRoiSize.width;
                        zoomState=END;
                    }
                    break;
                case MOVE:
                    scaleRect(roi,aspect,zoomSign*zoomSpeed);
                    if((abs(aimH-roi.height) < stopZoomThr) || cvRound(roi.height) <= aim.height)zoomState=END;
                    if(roi.height > maxRoiSize.height) {
                        roi.height=maxRoiSize.height;
                        roi.width=maxRoiSize.width;
                        zoomState=END;
                    }
                    break;
                case END:
                    zoomSpeed-=zoomSpeedInc;
                    scaleRect(roi,aspect,zoomSign*zoomSpeed);
                    if(zoomSpeed < minZoomSpeed) {zoomState=STOP; zoomSpeed=minZoomSpeed;}
                    if(roi.height > maxRoiSize.height) {
                        roi.height=maxRoiSize.height;
                        roi.width=maxRoiSize.width;
                        zoomState=STOP;
                        zoomSpeed=minZoomSpeed;
                    }
                    break;
                }
                }
                tmr.push_back(cvGetTickCount());if(frameCounter<2)lines.push_back(__LINE__);

                if(bMove) {
                    gp = getGoldenPoint(roi,aim);
                    moveX.update(roi.x,gp.x,roi.width/15.0);
                    moveY.update(roi.y,gp.y,roi.height/3.0);
                }
                tmr.push_back(cvGetTickCount());if(frameCounter<2)lines.push_back(__LINE__);

                if(roi.x<0) roi.x = 0;
                if(maxRoiSize.width < roi.x+roi.width)
                    roi.x = maxRoiSize.width-roi.width;
                if(roi.y<0)roi.y = 0;
                if(maxRoiSize.height < roi.y+roi.height)
                    roi.y=maxRoiSize.height-roi.height;
                /// end Motion and zoom
            }catch(Exception &mvEx){
                cout << "Motion and zoom block: "<< mvEx.msg << endl;
            }

            tmr.push_back(cvGetTickCount());if(frameCounter<2)lines.push_back(__LINE__);
            try{
                Rect2f roiFullSize = Rect2f(Point(roi.x*scale,roi.y*scale),Size(roi.width*scale,roi.height*scale));
                resize(fullFrame(roiFullSize), result , resultSize, 0,0, INTER_LINEAR );
                outputVideo << result ;
            }catch(Exception &mvEx){
                cout << "Result saving: "<< mvEx.msg << endl;
            }
            tmr.push_back(cvGetTickCount());if(frameCounter<2)lines.push_back(__LINE__);





            if(showPreview || recordPreview){ // Отрисовка области интереса
                resize(fullFrame, preview, smallImgSize, 0, 0, INTER_NEAREST );

                // Рисовать кадр захвата
                if(zoomState==START)rectangle(preview,roi,Scalar(100,255,100),thickness+stopZoomThr);
                if(zoomState==MOVE)rectangle(preview,roi,Scalar(255,255,255),thickness+stopZoomThr);
                if(zoomState==END)rectangle(preview,roi,Scalar(100,100,255),thickness+stopZoomThr);
                rectangle(preview,roi,Scalar(0,0,255),thickness);
                Scalar colorX;
                switch (moveX.getState()){
                case START:
                    colorX = Scalar(127,255,127);break;
                case MOVE:
                    colorX = Scalar(255,255,255);break;
                case END:
                    colorX = Scalar(127,127,255);break;
                }
                Scalar colorY;
                switch (moveY.getState()){
                case START:
                    colorY = Scalar(127,255,127);break;
                case MOVE:
                    colorY = Scalar(255,255,255);break;
                case END:
                    colorY = Scalar(127,127,255);break;
                }
                if(moveX.getState()!=STOP){
                    if(moveX.getSign()<0)
                        arrowedLine(preview,
                                    Point(roi.x,roi.y+roi.height/2),
                                    Point(roi.x-moveX.getSpeed()*2,roi.y+roi.height/2),
                                    colorX,thickness,8,0,1);
                    else
                        arrowedLine(preview,
                                    Point(roi.x+roi.width,roi.y+roi.height/2),
                                    Point(roi.x+roi.width+moveX.getSpeed()*2,roi.y+roi.height/2),
                                    colorX,thickness,8,0,1);
                }
                if(moveY.getState()!=STOP){
                    if(moveY.getSign()<0)
                        arrowedLine(preview,
                                    Point(roi.x+roi.width/2,roi.y),
                                    Point(roi.x+roi.width/2,roi.y-moveY.getSpeed()*2),
                                    colorY,thickness,8,0,1);
                    else
                        arrowedLine(preview,
                                    Point(roi.x+roi.width/2,roi.br().y),
                                    Point(roi.x+roi.width/2,roi.br().y+moveY.getSpeed()*2),
                                    colorY,thickness,8,0,1);
                }
                drawGoldenRules(preview,roi,Scalar(0,255,0),dotsRadius);
                // Рисовать цель
                rectangle(preview,aim,Scalar(0,255,0),thickness);
                putText(preview, "aim",aim.tl(),CV_FONT_NORMAL,fontScale,Scalar(0,255,0),textThickness);
                //Отрисовка распознаных объектов на превью
                drawRects(preview,facesFull,"Full face",Scalar(255,0,0),fontScale,textThickness,textOffset,thickness);
                drawRects(preview,facesProf,"Profile",Scalar(255,127,0),fontScale,textThickness,textOffset,thickness);

                /// Вывести время в превью
                time_t t = time(0);   // get time now
                struct tm * now = localtime( & t );
                stringstream timestring;
                timestring
                           << __DATE__ <<" "<< __TIME__
                           "|"<< (now->tm_year + 1900) << '.'
                           << (now->tm_mon + 1) << '.'
                           << now->tm_mday << " "
                           << now->tm_hour <<":"
                           << now->tm_min << ":"
                           << now->tm_sec;
//                           << frameCounter;
                putText(preview,timestring.str(),Point(0,smallImgSize.height-3),CV_FONT_NORMAL,fontScale*1.35,Scalar(0,0,0),textThickness*100);
                putText(preview,timestring.str(),Point(0,smallImgSize.height-3),CV_FONT_NORMAL,fontScale*1.35,Scalar(255,255,255),textThickness*2);
                stringstream frame;
                frame << frameCounter;
                putText(preview,(frame.str()),Point(0,fontScale*20),CV_FONT_NORMAL,fontScale*1.45,Scalar(0,0,0,100),textThickness*10);
                putText(preview,(frame.str()),Point(0,fontScale*20),CV_FONT_NORMAL,fontScale*1.45,Scalar(255,255,255),textThickness*5);
                tmr.push_back(cvGetTickCount());if(frameCounter<2)lines.push_back(__LINE__);
                // Сохранение кадра
                if(recordPreview)
                    previewVideo << preview;
                if(showPreview)
                    imshow(prevWindTitle.c_str(),preview);
            }
            tmr.push_back(cvGetTickCount());if(frameCounter<2)lines.push_back(__LINE__);

            cout <<"frame:"<< ++frameCounter
                << " fps:" << (int)(1000*(float)ticksPerMsec/(float)(tmr[tmr.size()-1]-tmr[0]))
                << " ("<< (int)((100*(float)frameCounter)/(float)videoLength) <<"% of video)"
                << endl;
            if( waitKey(1) == 27 )break;
            /// Запись статистики
            if(logFile.is_open()) {
                if(frameCounter<2){
                    logFile << "frame\t";
                    for (int i = 0; i < lines.size()-1; ++i) {
                        logFile << lines[i] << "-" << lines[i+1] << "\t";
                    }
                    logFile << "facesFull.x, px"<<"\t"
                            <<"facesFull.y, px"<<"\t"
                           << "facesFull.width, px"<<"\t"
                           <<"facesFull.height, px"<<"\t"
                          << "faceProf.x, px"<<"\t"
                          <<"faceProf.y, px"<<"\t"
                         << "facesProf.width, px"<<"\t"
                         <<"facesProf.height, px"<<"\t"
                        << "aim.x, px"<<"\t"
                        <<"aim.y, px"<<"\t"
                       << "aim.width, px"<<"\t"
                       <<"aim.height, px"<<"\t"
                      << "roi.x, px\t"
                      <<"roi.y, px"<<"\t"
                     << "roi.width, px\t"
                     <<"roi.height, px"<< endl;
                }
                logFile  << frameCounter <<"\t";
                for (int i = 0; i < tmr.size()-1; ++i) {
                    logFile << (double)(tmr[i+1]-tmr[i])/ticksPerMsec <<"\t";
                }
                if(!facesFull.empty())
                    logFile << facesFull[0].x << "\t"
                            << facesFull[0].y << "\t"
                            << facesFull[0].width << "\t"
                            << facesFull[0].height << "\t";
                else logFile << "\t\t\t\t";
                if(!facesProf.empty())
                    logFile << facesProf[0].x << "\t"
                            << facesProf[0].y << "\t"
                            << facesProf[0].width << "\t"
                            << facesProf[0].height << "\t";
                else logFile << "\t\t\t\t";

                logFile << aim.x << "\t"
                        << aim.y << "\t"
                        << aim.width << "\t"
                        << aim.height << "\t";
                logFile << roi.x << "\t"
                        << roi.y << "\t"
                        << roi.width << "\t"
                        << roi.height << endl;
            }

            tmr.clear();
            lines.clear();
        }
        if(logFile.is_open())logFile.close();
        cout << "The results have been written to " << "''"+outFileTitle.str()+"''" << endl;
        cvDestroyAllWindows();
    }
    else
    {
        cout << "Detecting face(s) in " << inputName << endl;
        if( !image.empty() )
        {
            detectAndDraw( image, cascadeFull, cascadeEyeL, scale, tryflip );
            waitKey(0);
        }
        else if( !inputName.empty() )
        {
            /* assume it is a text file containing the
            list of the image filenames to be processed - one per line */
            FILE* f = fopen( inputName.c_str(), "rt" );
            if( f )
            {
                char buf[1000+1];
                while( fgets( buf, 1000, f ) )
                {
                    int len = (int)strlen(buf), c;
                    while( len > 0 && isspace(buf[len-1]) )
                        len--;
                    buf[len] = '\0';
                    cout << "file " << buf << endl;
                    image = imread( buf, 1 );
                    if( !image.empty() )
                    {
                        detectAndDraw( image, cascadeFull, cascadeEyeL, scale, tryflip);
                        c = waitKey(0);
                        if( c == 27 || c == 'q' || c == 'Q' )
                            break;
                    }
                    else
                    {
                        cerr << "Aw snap, couldn't read image " << buf << endl;
                    }
                }
                fclose(f);
            }
        }
    }

    return 0;
}

void detectAndDraw( Mat& img, CascadeClassifier& cascade,
                    CascadeClassifier& nestedCascade,
                    double scale, bool tryflip)
{
    bool found_face=false;
    double t = 0;
    vector<Rect> faces, faces2;
    Mat gray, smallImg;
    static vector<Rect> facesBuf;
    const static Scalar colors[] =
    {
        Scalar(255, 0,      0),
        Scalar(255, 128,    0),
        Scalar(255, 255,    0),
        Scalar(0,   255,    0),
        Scalar(0,   128,    255),
        Scalar(0,   255,    255),
        Scalar(0,   0,      255),
        Scalar(255, 0,      255)
    };

    cvtColor( img, gray, COLOR_BGR2GRAY );
    double fx = 1 / scale;
    resize( gray, smallImg, Size(), fx, fx, INTER_LINEAR );
    equalizeHist( smallImg, smallImg );
    t = (double)cvGetTickCount();
    cascade.detectMultiScale( smallImg, faces,
        1.1, 2, 0
        // |CASCADE_FIND_BIGGEST_OBJECT
        //|CASCADE_DO_ROUGH_SEARCH
        |CASCADE_SCALE_IMAGE,
        Size(30, 30) );
    if( tryflip )
    {
        flip(smallImg, smallImg, 1);
        cascade.detectMultiScale( smallImg, faces2,
                                 1.1, 2, 0
                                 // |CASCADE_FIND_BIGGEST_OBJECT
                                 //|CASCADE_DO_ROUGH_SEARCH
                                 |CASCADE_SCALE_IMAGE,
                                 Size(30, 30) );
        for( vector<Rect>::const_iterator r = faces2.begin(); r != faces2.end(); r++ )
        {
            faces.push_back(Rect(smallImg.cols - r->x - r->width, r->y, r->width, r->height));
        }
    }
    if(faces.size()>0){
        found_face=true;
        // facesBuf=faces;
        // printf( "after flippping %d faces", faces.size() );
    }
    t = (double)cvGetTickCount() - t;
    printf( "detection time = %g ms\n", t/((double)cvGetTickFrequency()*1000.) );
    Rect roiRect;
    static Rect closeUpROI(0,0,320,240);
    for ( size_t i = 0; i < faces.size(); i++ )
    {
        Mat smallImgROI;
        Rect r = faces[i];
        vector<Rect> nestedObjects;
        Point center;
        Scalar color = colors[i%8];
        int radius;

        double aspect_ratio = (double)r.width/r.height;
        if( 0.75 < aspect_ratio && aspect_ratio < 1.3 )
        {
            center.x = cvRound((r.x + r.width*0.5)*scale);
            center.y = cvRound((r.y + r.height*0.5)*scale);
            radius = cvRound((r.width + r.height)*0.25*scale);
            circle( img, center, radius, color, 3, 8, 0 );
        }
        else
            rectangle( img, cvPoint(cvRound(r.x*scale), cvRound(r.y*scale)),
                       cvPoint(cvRound((r.x + r.width-1)*scale), cvRound((r.y + r.height-1)*scale)),
                       color, 3, 8, 0);
        if( nestedCascade.empty() )
            continue;

        smallImgROI = smallImg( r );
        nestedCascade.detectMultiScale(smallImgROI, nestedObjects,
            1.1, 2, 0
//            |CASCADE_FIND_BIGGEST_OBJECT
            |CASCADE_DO_ROUGH_SEARCH
            //|CASCADE_DO_CANNY_PRUNING
            |CASCADE_SCALE_IMAGE,
            Size(30, 30) );
        for ( size_t j = 0; j < nestedObjects.size(); j++ )
        {
            Rect nr = nestedObjects[j];
            center.x = cvRound((r.x + nr.x + nr.width*0.5)*scale);
            center.y = cvRound((r.y + nr.y + nr.height*0.5)*scale);
            radius = cvRound((nr.width + nr.height)*0.25*scale);
            circle( img, center, radius, color, 3, 8, 0 );
        }
    }
    imshow( "result", img );
    /*static Mat frameCloseUpLast;
    static Mat motionCloseUp;
    frameCloseUpLast = closeUpROI.clone();*/
    //Выводим крупный план в отдельное окно
    for ( size_t i = 0; i < facesBuf.size(); i++ ){
        if(facesBuf[i].x+closeUpROI.width < img.cols) {
            closeUpROI.x=facesBuf[i].x;
        }else{
            closeUpROI.x=img.cols-closeUpROI.width;
        }
        if(facesBuf[i].y+closeUpROI.height < img.rows){
            closeUpROI.y=facesBuf[i].y;
        }else{
            closeUpROI.y=img.rows-closeUpROI.height;
        }
        string title = "Face";
        std::string s;
        std::stringstream out;
        out << i;
        title += out.str();

        imshow( title, img(closeUpROI) );
    }
    // return facesBuf;
}




