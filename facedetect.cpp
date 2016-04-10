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

inline void scaleRect(Rect& r, const double& sc) {
    r.x *= sc;
    r.y *= sc;
    r.height *= sc;
    r.width *= sc;
}
Point getGoldenPoint(Rect roi, Rect face){
        static Point leftUp(roi.width/3.0, roi.height/3.0);
    static Point leftDown(roi.width/3.0, 2.0*roi.height/3.0);
    static Point rightUp(2.0*roi.width/3.0,roi.height/3.0);
    static Point rightDown(2.0*roi.width/3.0,2.0*roi.height/3.0);
    static Point center(roi.width/2,roi.height/3);
    Point target;
    if(roi.width/3.0 - face.width < 0 ) /// если лицо крупное, то держать его в центре кадра
        target = center;
    else if(face.x+face.width/2.0 < center.x
            && face.x < roi.x+leftUp.x)
        target = leftUp;
    else if(face.x+face.width > roi.x+rightUp.x) // Камера посередине не будет реагировать
        target = rightUp;
    else
        target = center;
    int x=(face.x+face.width/2.0-target.x);
    int y=(face.y+face.height/3.0 - target.y);
    return Point(x,y);
}
inline Rect middle(const Rect& a, const Rect& b){
    Rect mid;
    mid.x = (a.x + b.x)/2;
    mid.y = (a.y + b.y)/2;
    mid.width = (a.width + b.width)/2;
    mid.height = (a.height + b.height)/2;
    return mid;
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

inline Point rcenter(const Rect& r){
    return Point(cvRound(r.x+r.width*0.5) , cvRound(r.y+r.height*0.5));
}

void autoZoom(const Rect& face,
              Rect& roi,
              float aspectRatio, int maxStep, const double& relation){
       static PIDController pidZ(0.025,0.001,-0.09,maxStep);
       int dh = pidZ.getU(face.height*relation - roi.height);
       Point pb = rcenter(roi);
       roi.height += dh;
       roi.width = (roi.height*aspectRatio);
       Point pa = rcenter(roi);
       roi -= (pa-pb);

}
void autoMove(const Rect& face,
                     Rect& roi,
                     const int& maxWidth,
                     const int& maxHeight)
{
    static PIDController pidX(0.1, 0.001, -0.09,(double)maxWidth/100.0);
    static PIDController pidY(0.1, 0.001, -0.09,(double)maxWidth/100.0);

    Point p(getGoldenPoint(roi,face));
    roi.x += pidX.getU(p.x-roi.x);
    roi.y += pidY.getU(p.y-roi.y);
}

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

inline void drawGoldenRules(Mat& img, const Rect& r,Scalar color=Scalar(0,255,0),const double& dotsRadius=1){
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

        const int previewHeight = 480;
        const Size fullFrameSize = Size((int) capture.get(CV_CAP_PROP_FRAME_WIDTH),
                  (int) capture.get(CV_CAP_PROP_FRAME_HEIGHT));
        const Rect fullShot(Point(0,0),fullFrameSize);
        float aspectRatio = (float)fullShot.width/(float)fullShot.height;
        const int maxStep = fullShot.width*0.8;
        const float frameRatio= (float)fullFrameSize.width / (float)fullFrameSize.height;
        const Size roiSize = Size((int)(fullFrameSize.width/3.0),(int)(fullFrameSize.height/3.0));
        const Size previewSmallSize = Size((int)(previewHeight*frameRatio),previewHeight);
        const double fx = 1 / scale;
        const double ticksPerMsec=cvGetTickFrequency() * 1.0e3;
        const float thickness = 3.0*(float)fullFrameSize.height/(float)previewSmallSize.height;
        const int stabThresh = 15.0*(float)fullFrameSize.height/(float)previewSmallSize.height;
        const int textOffset = thickness*2;
        const int textThickness = thickness/2;
        const int dotsRadius = thickness*2;
        const double fontScale = thickness/5.0;

        stringstream outFileTitle;

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
        int fps;
        int fourcc;
        if(isWebcam){
            fps = capture.get(CAP_PROP_FPS)/5.0;
            fourcc = VideoWriter::fourcc('M','J','P','G'); // codecs
        }
        else{
            fourcc = capture.get(CV_CAP_PROP_FOURCC); // codecs
            fps = capture.get(CAP_PROP_FPS);
        }
        long int videoLength = capture.get(CAP_PROP_FRAME_COUNT);
        long int frameCounter=1;
        VideoWriter outputVideo("results/closeUp_"+outFileTitle.str()+".avi",fourcc,
                                 fps, roiSize, true);
        if(!outputVideo.isOpened()){
            cout << "Could not open the output video ("
                 << "results/closeUp_"+outFileTitle.str()+".avi" <<") for writing"<<endl;
            return -1;
        }
        VideoWriter previewVideo;
        if(recordPreview){
             if(!previewVideo.open("results/test_"+outFileTitle.str()+".avi",fourcc,
                                   fps, previewSmallSize, true))
             {
                 cout << "Could not open the output video ("
                      << "results/test_"+outFileTitle.str()+".avi" <<") for writing"<<endl;
                 return -1;
             }
        }
        fstream logFile(("results/test_"+outFileTitle.str()+".csv").c_str(), fstream::out);
        if(!logFile.is_open()){
            cout << "Error with opening the file:" << "results/test_"+outFileTitle.str()+".csv" << endl;
        }else{
            logFile << "frame\t"
                    << "timestamp, ms\t"
                << "faceDetTime, ms\t"
                << "motDetTime, ms\t"
                << "updateTime, ms\t"
                << "oneIterTime, ms\t"
                << "faces[0].x, px"<<"\t"<<"faces[0].y, px"<<"\t"
                << "roi.x, px\t"<<"roi.y, px"<<"\t" << endl;
        }


        Mat previewSmall, previewFrame,gray,smallImg;
        Mat frameTemp;
        bool motionDetected=true;

        Rect roi;
        vector<Rect> facesFull,facesProf,eyesL,eyesR;
        vector<Rect> faceBuf;
        int64 oneIterEnd, oneIterStart,motDetStart,motDetEnd,faceDetStart,faceDetEnd,updateStart,updateEnd, timeStart;
        vector<int64> tStamps;
        vector<int> lines;
        double oneIterTime, motDetTime, faceDetTime,updateTime,timeEnd;
        string title = "Small preview";

        const int minNeighbors=1; //количество соседей
        const double scaleFactor=1.25;
        const Size minfaceSize=Size(25,25);
        timeStart = cvGetTickCount();
        Mat result;
        bool foundFaces=false;
        Rect aim=fullShot;
        roi = fullShot;
        unsigned int aimUpdateFreq=5;
        float step=1.5;
        double relation = FI;
        bool bZoom=false;
        if(scaleFactor<1.01)return -1;

  ////////////////////////////////////////////////
        for(;;)
        {
            oneIterStart = cvGetTickCount(); 
            capture >> frame;
            cout <<"frame:"<< ++frameCounter << " ("<<(int)((100*(float)frameCounter)/(float)videoLength) <<"% of video)"<< endl;
            if(frame.empty()) {
                clog << "Frame is empty" << endl;
                break;
            }
            previewFrame = frame.clone();

            faceDetStart = cvGetTickCount();
            cvtColor( frame, gray, COLOR_BGR2GRAY );
            resize( gray, smallImg, Size(), fx, fx, INTER_LINEAR );

            /* Поиск лиц в анфас */
            cascadeFull.detectMultiScale(smallImg, facesFull,
                scaleFactor, minNeighbors, 0|CASCADE_SCALE_IMAGE,minfaceSize);
            /// Поиск лиц в профиль
            cascadeProf.detectMultiScale( smallImg, facesProf,
                scaleFactor, minNeighbors, 0|CASCADE_SCALE_IMAGE,minfaceSize);

            for (size_t i=0; i<facesFull.size(); ++i) {
                scaleRect(facesFull[i],scale);
            }
            for (size_t i=0; i<facesProf.size(); ++i) {
                scaleRect(facesProf[i],scale);
            }
            foundFaces = !(facesFull.empty() && facesProf.empty());
            //Отрисовка распознаных объектов на превью
            if(showPreview || recordPreview){
                drawRects(previewFrame,facesFull,"Full face",Scalar(255,0,0));
                drawRects(previewFrame,facesProf,"Profile",Scalar(255,127,0));
            }
            faceDetEnd = cvGetTickCount();
            updateStart = cvGetTickCount();
            if(frameCounter%aimUpdateFreq == 0){
                if(!facesProf.empty()) aim = facesProf[0];
                if(!facesFull.empty()) aim = facesFull[0];
                if(showPreview || recordPreview){
                    rectangle(previewFrame,aim,Scalar(0,255,0),thickness);
                    putText(previewFrame, "aim",aim.tl(),CV_FONT_NORMAL,1.0,Scalar(0,255,0),thickness);
                }
            }

            /// Motion and zoom
            float aimH = aim.height*relation;
            if(( aimH>(float)roi.height*step || aimH<(float)roi.height/step) && !bZoom)bZoom=true;
            if((abs(cvRound(aimH)-roi.height) < stabThresh) && bZoom)bZoom=false;
            if(bZoom) autoZoom(aim,roi,aspectRatio,maxStep,relation);

            autoMove(aim,roi,fullFrameSize.width,fullFrameSize.height);

            if(roi.area() > fullShot.area()) {
                roi.height=fullShot.size().height;
                roi.width=fullShot.size().width;
            }
            if(roi.x<0) roi.x = 0;
            else if(fullFrameSize.width < roi.x+roi.width)
                roi.x = fullFrameSize.width-roi.width;
            if(roi.y<0)roi.y = 0;
            else if(fullFrameSize.height < roi.y+roi.height)
                roi.y=fullFrameSize.height-roi.height;
            /// end Motion and zoom
            updateEnd = cvGetTickCount();

            motDetStart = cvGetTickCount();
            motDetEnd = cvGetTickCount();

            resize(frame(roi), result , roiSize, 0,0, INTER_LINEAR );
            outputVideo << result ;

            if(showPreview || recordPreview){ // Отрисовка области интереса
                if(bZoom)rectangle(previewFrame,roi,Scalar(127,127,127),stabThresh,LINE_AA);
                rectangle(previewFrame,roi,Scalar(0,0,255),thickness);
                drawGoldenRules(previewFrame,roi,Scalar(0,255,0),dotsRadius);
                /// Вывести время в превью
                time_t t = time(0);   // get time now
                struct tm * now = localtime( & t );
                stringstream timestring;
                 timestring << (now->tm_year + 1900) << '.'
                              << (now->tm_mon + 1) << '.'
                              << now->tm_mday << " "
                              << now->tm_hour <<":"
                              << now->tm_min << ":"
                              << now->tm_sec << "."
                              << frameCounter<< " build:"
                              << __DATE__ <<" "<< __TIME__ ;

                resize( previewFrame, previewSmall, previewSmallSize, 0, 0, INTER_NEAREST );
                putText(previewSmall,timestring.str(),Point(0,previewSmallSize.height-3),CV_FONT_NORMAL,0.7,Scalar(0,0,0),5);
                putText(previewSmall,timestring.str(),Point(0,previewSmallSize.height-3),CV_FONT_NORMAL,0.7,Scalar(255,255,255));
                if(recordPreview)
                    previewVideo << previewSmall;
                if(showPreview)
                    imshow(title.c_str(),previewSmall);
            }

            int c = waitKey(10);
            if( c == 27 || c == 'q' || c == 'Q' )break;
    
            /// Запись статистики
            oneIterEnd = cvGetTickCount(); 
            timeEnd = (double)(cvGetTickCount() - timeStart)/ticksPerMsec;
            faceDetTime = (double)(faceDetEnd - faceDetStart)/ticksPerMsec;
            updateTime =  (double)(updateEnd - updateStart)/ticksPerMsec;
            motDetTime  = (double)(motDetEnd - motDetStart)/ticksPerMsec;
            oneIterTime = (double)(oneIterEnd - oneIterStart)/ticksPerMsec;
            if(logFile.is_open()) {
                logFile  << frameCounter << "\t"
                         << timeEnd << "\t"
                    << faceDetTime << "\t" 
                    << motDetTime << "\t" 
                    << updateTime << "\t"
                    << oneIterTime << "\t";
                if(!facesFull.empty())
                    logFile << facesFull[0].x << "\t"
                                     << facesFull[0].y << "\t"; else logFile << "\t\t";
                 logFile << roi.x << "\t"
                                    << roi.y << "\t";
                logFile  << endl;
            }
            oneIterEnd = faceDetEnd = motDetEnd = -1;
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




