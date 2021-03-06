/**
  * \brief Программа для детекции и трекинга лиц в видео с повышенным разрешением
  * \author Ilya Petrov
  */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <signal.h>
#include <stdio.h>
#include <chrono>
#include "opencv2/objdetect.hpp"
#include "opencv2/highgui.hpp"
#include "opencv2/imgproc.hpp"
#include <iostream>
#include <iomanip>
#include <string>
#include <sstream>
#include <fstream>
#include <cstdlib>
#include <opencv2/video/video.hpp>  // Video write
#include <ctime>
#include "pthread.h"
#include <stdio.h>
#include <stdlib.h>

#include "automotion.h"
#include "autozoom.h"
#include "autocamera.h"
#include "arg.h"
#include "detector.h"
#include "3rdparty/asms/colotracker.h"
#include "preview.h"
#include "viewfinder.h"
#include <exception>
using namespace std;
using namespace cv;
Mat fullFrame;
Mat* result;

const double FI=1.61803398; /// Золотое сечение

long int frameCounter=0; /// \todo 18.05.2016 class InputMan
typedef cv::Point3_<uint8_t> Pixel;

void stream(Mat mat, FILE* f){
    if(!mat.empty()){
        fflush(f);
//        fwrite(new int(0),sizeof(uchar),1,f); //red
//        fwrite(new int(0),sizeof(uchar),1,f); //blue
//        fwrite(new int(0),sizeof(uchar),1,f);//green
        for (Pixel &p : cv::Mat_<Pixel>(mat)) {
//            fwrite(new int(255),sizeof(p.x),1,f); //red
            fwrite(&p.x,sizeof(uchar),1,f);
//            fwrite(new int(0),sizeof(p.y),1,f); //blue
//            fwrite(new int(0),sizeof(p.z),1,f);//green
            fwrite(&p.y,sizeof(uchar),1,f);
            fwrite(&p.z,sizeof(uchar),1,f);
        }
    }
}
void streamInThread(atomic<bool>& program_is_running, const char *filename, Mat* mat){
    FILE* fifoFdRes;
    int fifresult=0;
    try{
//        unlink(filename);
        fifresult = mkfifo(filename,O_RDWR|S_IRWXU|O_NONBLOCK);
        fifoFdRes = fopen(filename,"w");
        while(program_is_running){
            if(mat!=NULL) {
                stream(mat->clone(),fifoFdRes);
            }
        }
        clog << __LINE__ << endl;
    }catch(exception e){
        cerr << "Exeption in " << __FUNCTION__ << ":" << e.what() <<endl;
    }
    clog << __LINE__ << endl;
    fclose(fifoFdRes);
    clog << __LINE__ << endl;
//    unlink(filename);
    clog << ">thread " << filename << " finished" << endl;
}

void captureFrame(atomic<bool>& program_is_running, string inputName){
    VideoCapture capture;
    bool isWebcam=false;
    if(!capture.isOpened()){
        if( inputName.empty() || (isdigit(inputName.c_str()[0]) && inputName.c_str()[1] == '\0') )
        {
            isWebcam=true;
            int c = inputName.empty() ? 0 : inputName.c_str()[0] - '0' ;
            if(!capture.open(c))
                clog << "Capture from camera #" <<  c << " didn't work" << endl;
        }else{
            if(capture.open(inputName)){
                clog << "Capture file " <<  inputName << endl;
                isWebcam=false;
            }
            else
                clog << "Could not capture file " <<  inputName << endl;
        }
    }


    if(capture.isOpened() )
    {
        int64 ct=0,pt=0;
        capture.set(CV_CAP_PROP_BUFFERSIZE,1024);
        clog << "fps:" << capture.get(CV_CAP_PROP_FPS) << endl;
        int fps = capture.get(CV_CAP_PROP_FPS);
        if(fps==0)fps=25;
        int64 delay=0;
        int t=0;
        int fDur = 1000000/fps; ///< frame Duration (in microseconds 10^-6)
        while(program_is_running){
            if(capture.grab()){
                if(delay < fDur){
                    capture.retrieve(fullFrame); /// \todo 13.02.2017 Добавить мьютекс или семафор
                }else{
                    capture.grab();
                    delay=0;
                    clog << "frame\t" << frameCounter << "\tdropped" << endl;
                    ++frameCounter;
                }
                ++frameCounter;
            }

            pt=ct;
            ct=cvGetTickCount();
            t=(ct-pt)/cvGetTickFrequency();
            delay+=(fDur - t);
            if(delay<0)delay=0;
            cout << "frameDuration: "<< t <<" "<< fDur << "(fDur) delay:" << delay << endl;
        }
    }
    clog << ">thread Capture finished" << endl;
}

static void help()
{
    clog << "Build date:" << __DATE__ << " " << __TIME__
            "\n\tDuring execution:\n\tHit any key to quit.\n"
            "\tUsing OpenCV version " << CV_VERSION << "\n" << endl;
}
/**
 * @brief Главная функция
 * @param[in] argc Количество аргументов в программе
 * @param[in] argv Массив аргументов
 * @return 0, если программа завершена удачно
 */

int main( int argc, const char** argv )
{

    string inputName; ///< Путь к файлу видео для обработки. Если не введен, то изображение захватывается с камеры /// \todo 18.05.2016 class FileSaver

    clog << "Availible parameters: " << endl;
    /// Параметры детектора лиц
    CascadeClassifier cascadeFull,cascadeProf; ///< Каскады Хаара для детекции лица /// \todo 18.05.2016 class Detector
    string cascadeFullName;// = "--cascadeFront=../haarcascade_frontalface_alt.xml";
    string cascadeProfName;// = "--cascadeFront=../haarcascade_frontalface_alt.xml --cascadeProf=../haarcascade_profileface.xml";
    const string cascadeProfOpt = "--cascadeProf=";
    size_t cascadeProfOptLen = cascadeProfOpt.length();
    const string cascadeFullOpt = "--cascadeFront=";
    size_t cascadeFullOptLen = cascadeFullOpt.length();
    int manualInput=0;
    Arg<double> scale(argv, argc,manualInput,3,"--scale=","%lf", new double(1)); ///< Этот параметр отвечает за то, во сколько раз следует сжать кадр перед тем как приступить к детекции лица
    Arg<int>minNeighbors(argv, argc,manualInput,1,"--minNeighbors=","%d",new int(1)); ///<  Количество соседних детекций лица в изображении
    Arg<double> scaleFactor(argv, argc,manualInput,1.1,"--scaleFactor=","%lf", new double(1.1));/**< шаг изменения размеров лица, которое ожидается детектировать
                                                                            в изображении.Чем ближе этот параметр к единице,
                                                                        тем точнее будет определён размер лица, но тем дольше будет работать алгоритм*/
    Arg<int> minFaceHeight(argv, argc,manualInput,25,"--minFaceHeight=","%d", new int(1));///< Минимальные размеры детектируемого лица
    Arg<int> aimUpdatePer(argv, argc,manualInput,15,"--aimUpdatePer=","%d",new int(1));///< Период обновления цели (каждые n кадров), к которой будет следоватьвиртуальная камера
    Arg<int> faceDetectPer(argv, argc,manualInput,1,"--faceDetectPer=","%d", new int(1));///< Период детектирования лиц

    Arg<int> aimCheckerPer(argv, argc,manualInput,10,"--aimCheckerPeriod=","%d",new int(1)); ///< период редетекции лица внутри прямоугольника, который создаёт трекер/ задаётся в кадрах
    Arg<unsigned int> detWarningLimit(argv, argc,manualInput,3,"--detWarningLimit=","%d",new unsigned int(1));
    Arg<float> focusEx_(argv, argc,manualInput,0.3,"--aimEx=","%f",new float(0.0), new float(1.0)); /// на сколько процентов от высоты исходного кадра расширять область редетекции, чтобы попытаться найти цель

    ///Запись результата
    Arg<int> resultWidth(argv, argc,manualInput,640,"--resultWidth=","%d", new int(1));///< Ширина результирующего видео (ширина рассчитывается автоматически в соответствии с соотношением сторон)
    Arg<int> resultHeight(argv, argc,manualInput,480,"--resultHeight=","%d", new int(1));///< Высота результирующего видео (ширина рассчитывается автоматически в соответствии с соотношением сторон)

    Arg<int> previewWidth(argv, argc,manualInput,resultWidth,"--previewWidth=","%d", new int(1));///< Ширина превью видео (ширина рассчитывается автоматически в соответствии с соотношением сторон)
    Arg<int> previewHeight(argv, argc,manualInput,resultHeight,"--previewHeight=","%d", new int(1));///< Высота превью видео (ширина рассчитывается автоматически в соответствии с соотношением сторон)


    Arg<int> recordResult(argv, argc,manualInput,1,"--recordResult=","%d",new int(0));///< Записывать результирующее видео.
    Arg<int> writeCropFile(argv, argc,manualInput,0,"--writeCropFile=","%d",new int(0));///< Записывать фильтр-скрипт для обработки исходного видео в ffmpeg (см. [filter_script](http://ffmpeg.org/ffmpeg.html#Main-options))

    /// Визуализация
    Arg<int> showResult(argv, argc,manualInput,0,"--showResult=","%d",new int(0));
    Arg<int> showPreview(argv, argc,manualInput,0,"--showPreview=","%d",new int(0));///< Показывать в реальном времени процесс обработки видео с отрисовкой виртуальной камеры и детектированных лиц
    Arg<int> recordPreview(argv, argc,manualInput,0,"--recordPreview=","%d",new int(0));///< Записывать процесс обработки видео в отдельный файл

    /// Перемещение виртуальной камеры
    Arg<float>maxStepX(argv, argc,manualInput,1,"--maxStepX=","%f",new float(0.2));///< Максимальная скорость по координате Х
    Arg<float>maxStepY(argv, argc,manualInput,1,"--maxStepY=","%f",new float(0.2));///< Максимальная скорость по координате У
    /// Зум
    Arg<float> zoomStartThr(argv, argc,manualInput,0.1,"--zoomThr=","%f",new float(0.001), new float(0.9999));///< zoomStartThr показывает насколько точно масштабирование/зум должны совпасть с целевым 1- должно быть один в один; 3 - может быть чуть больше или чуть меньше aimH>roi.height*zoomThr в относительных единицах
    Arg<double> face2shot(argv, argc,manualInput,FI,"--face2shot=","%lf",new double(0.1));///< Требуемое отношение высоты лица к высоте кадра
    Arg<double> zoomSpeedMin(argv, argc,manualInput,0.00,"--zoomSpeedMin=","%lf",new double(0.00));///< Минимальная скорость зума
    Arg<double> zoomSpeedMax(argv, argc,manualInput,1.00,"--zoomSpeedMax=","%lf",new double(0.001));///< Максимальная скорость зума
    Arg<double> zoomSpeedInc_(argv, argc,manualInput,0.1,"--zoomSpeedInc=","%lf",new double(0.001));///< Инкремент скорости зума

    Arg<int> streamResult(argv, argc,manualInput,0,"--streamResult=","%d",new int(0)); ///< Печатать кадры результирующего видео в стандартный вывод
    Arg<int>writeLogFile(argv, argc,manualInput,0,"--writeLogFile=","%d",new int(0),new int(1));

    Arg<int> streamPreview(argv, argc,manualInput,0,"--streamPreview=","%d",new int(0));
    help();

    /// Чтение аргументов программы
    for( int i = 1; i < argc; i++ )
    {
        clog << "\nProcessing " << i << " " <<  argv[i];

        if( cascadeFullOpt.compare( 0, cascadeFullOptLen, argv[i], cascadeFullOptLen ) == 0 )
        {
            cascadeFullName.assign( argv[i] + cascadeFullOptLen );
            clog << " assigned:" << cascadeFullOpt << cascadeFullName << endl;
        }
        else if( cascadeProfOpt.compare( 0, cascadeProfOptLen, argv[i], cascadeProfOptLen ) == 0 )
        {
            clog << "\n nc: " <<endl;
            //if( argv[i][cascadeProfOpt.length()] == '=' )
            cascadeProfName.assign( argv[i] + cascadeProfOptLen );
            clog << " assigned:" << cascadeProfOpt << cascadeProfName << endl;


        }
        else if(argv[i][0]!='-' && argv[i][1]!='-') inputName.assign( argv[i] );
    }
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

    bool isWebcam=false; /// \todo 18.05.2016 class InputMan
    VideoCapture capture;
    while(!capture.isOpened()){
        if( inputName.empty() || (isdigit(inputName.c_str()[0]) && inputName.c_str()[1] == '\0') )
        {
            isWebcam=true;
            int c = inputName.empty() ? 0 : inputName.c_str()[0] - '0' ;
            if(!capture.open(c))
                clog << "Capture from camera #" <<  c << " didn't work" << endl;
        }else{
            if(capture.open(inputName)){
                clog << "Capture file " <<  inputName << endl;
                isWebcam=false;
            }
            else
                clog << "Could not capture file " <<  inputName << endl;
        }
    }
    ///Создание нити захвата кадров
    std::atomic<bool> running { true } ;
    thread captureThread(captureFrame,ref(running),inputName);
    if(isWebcam) writeCropFile=false; /// \todo 18.05.2016 class FileSaver
    clog << "Video capturing has been started ..." << endl;

    //    MODEL    //
    // Кадры
    /// Характеристики видео
    const long int videoLength = /*isWebcam ? 1 :*/ capture.get(CAP_PROP_FRAME_COUNT);
    /// \todo 05/12/2016 При стриминге размеры кадра определяются почему-то неправильно
    const Rect2f fullShot(0,0,(int)capture.get(CV_CAP_PROP_FRAME_WIDTH),
                          (int)capture.get(CV_CAP_PROP_FRAME_HEIGHT));
    int fps; ///< Количество кадров в секунду /// \todo 18.05.2016 class FileSaver, class Previewer
    int fourcc; ///< Код кодека, состоящий из 4-х символов (см. \ref fourcc.org http://www.fourcc.org/codecs.php)


    const Size resultSize = Size(resultWidth,resultHeight);
    const Size previewSize = Size(previewWidth, previewHeight);
    ///Zoom & movement params (driver)
    Detector det(cascadeFullName,cascadeProfName,
                 Size((float)fullShot.width/scale, (float)fullShot.height/scale),
                 aimUpdatePer,faceDetectPer,minNeighbors,minFaceHeight,scaleFactor,scale);
    AutoCamera cam(fullShot.size(),maxStepX,maxStepY,zoomSpeedMin,zoomSpeedMax,zoomStartThr,zoomSpeedInc_,face2shot,1,1); /// \todo 28.01.2017 убрать отсюда параметр scale. См. класс Detector

    ColorTracker* tracker = NULL;
    BBox* bb = NULL;
    bool bTrackerInitialized = false;
    Rect aim;
    Rect focus;


    //file writing
    stringstream outTitleStream;
    string outFileTitle;
    VideoWriter previewVideo;
    VideoWriter outputVideo;


    ///Test items
    fstream logFile;
    stringstream pzoom;


    //    VIEW    //
    Rect2f roiFullSize;
    //drawing
    Preview pre(previewSize.width,previewSize.height,"Preview");
    /// \todo 18.05.2016 end class Drawer ///

    // SetUp
    if(isWebcam){ /// \todo 18.05.2016 class InputMan
        time_t t = time(0);   // get time now
        struct tm * now = localtime( & t );
        outTitleStream << "webcam"
                       << (now->tm_year + 1900) << '_'
                       << (now->tm_mon + 1) << '_'
                       << now->tm_mday << "_"
                       << now->tm_hour <<"-"
                       << now->tm_min << "_"
                       << __DATE__ <<"_"<< __TIME__ <<"_arg"
                          //                          << scale
                          //                          << minNeighbors
                          //                          << scaleFactor
                          //                          << minFaceHeight
                          //                          << aimUpdatePer
                          //                          << faceDetectPer
                          //                          << resultHeight
                          //                          << showPreview
                          //                          << recordPreview
                          //                          << maxStepX
                          //                         << maxStepY
                          //                         << writeCropFile
                          //                         << recordResult
                          //                         << zoomStartThr
                          //                         << face2shot
                          //                         << zoomSpeedMin
                          //                         << zoomSpeedMax
                       << zoomSpeedInc_;
    }else{
        outTitleStream << inputName.substr(inputName.find_last_of('/')+1)
                       << "_" <<__DATE__ <<"_"<< __TIME__<<"_arg"
                          //            << scale<< "_"
                          //            << minNeighbors << "_"
                          //            << scaleFactor << "_"
                          //            << minFaceHeight << "_"
                          //            << aimUpdatePer << "_"
                          //            << faceDetectPer << "_"
                          //            << resultHeight << "_"
                          //            << showPreview << "_"
                          //            << recordPreview << "_"
                          //            << maxStepX << "_"
                          //           << maxStepY << "_"
                          //           << writeCropFile << "_"
                          //           << recordResult << "_"
                          //           << zoomStartThr << "_"
                          //           << face2shot << "_"
                          //           << zoomSpeedMin << "_"
                          //           << zoomSpeedMax << "_"
                       << zoomSpeedInc_;
    }
    outFileTitle=outTitleStream.str();
    replace(outFileTitle.begin(),outFileTitle.end(),' ','_');
    replace(outFileTitle.begin(),outFileTitle.end(),':','-');

    if(isWebcam){
        fps = capture.get(CAP_PROP_FPS);
        fourcc = VideoWriter::fourcc('M','P','4','2'); // codecs
    }
    else{
        fourcc = capture.get(CV_CAP_PROP_FOURCC); // codecs
        fps = capture.get(CAP_PROP_FPS);
    }
    result = new Mat(resultWidth,resultHeight,CV_8UC3);
    char fifotitle[] = "result";
    pid_t pidVlc;
    if((pidVlc=fork())==0){
        clog << "starting Vlc with pid:" << getpid() << endl;
        if(execlp("vlc","vlc",fifotitle,
                  "--demux=rawvideo",
                  "--rawvid-fps=25",
                  "--rawvid-width=854",
                  "--rawvid-height=480",
                  "--rawvid-chroma=RV24",
                  "--sout '#transcode{vcodec=mp4v,acodec=mpga,vb=2000,ab=32}:rtp{sdp=rtsp://:5025/dptz.sdp}'",
                  NULL
                  )==-1){
            switch (errno) {
            case E2BIG:
                cerr << "Слишком длинный список аргументов"<< endl;
                terminate();
                break;
            case EACCES:
                cerr << "- Отказ доступа."<< endl;
                break;
            case EMFILE:
                cerr << "- Слишком много открытых файлов."<< endl;
                break;
            case ENOENT:
                cerr << "- Маршрут доступа (PATH) или имя файла не найдены."<< endl;
                break;
            case ENOEXEC:
                cerr << "- Ошибка формата EXEC."<< endl;
                break;
            case  ENOMEM:
                cerr << "- Не хватает памяти."<< endl;
            default:
                cerr <<"errno:" << errno << endl;
                break;
            }
            exit(1);
        }
    }
    clog << "pidVlc:" << pidVlc << endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));//ждём, пока VLC запустится

    std::thread resultThread;
    if(streamResult){
        resultThread = thread(streamInThread,ref(running), fifotitle,result);
    }



    if(recordResult){
        if(!outputVideo.open("rtsp://:5252/result.sdp",fourcc,
                             fps, resultSize, true)){
            cerr << "Could not open the output video for writing"<<endl;
            recordResult=false;
            return -1;
        }
    }
    if(recordPreview){
        if(!previewVideo.open("../results/test_"+outFileTitle+".avi",fourcc,
                              fps, previewSize, true))
        {
            cerr << "Could not open the output video ("
                 << "../results/test_"+outFileTitle+".avi" <<") for writing"<<endl;
            recordPreview=false;
            //                 return -1;
        }
    }
    if(writeLogFile){
        logFile.open(("../results/test_"+outFileTitle+".ods").c_str(), fstream::out); /// \todo 18.05.2016 class StatMan
        if(!logFile.is_open()){
            clog << "Error with opening the file:" << "../results/test_"+outFileTitle+".ods" << endl;
        }
    }
    clog << "!!!";
    vector<Mat> frames;
    bool end=false;
    fstream cropFile;
    if(writeCropFile){ /// \todo 18.05.2016 class FileSaver
        cropFile.open("../results/filter",fstream::out);
    }

    unsigned int detWarning=0;
    int64 startIterationTime, endIterationTime;
    int64 startCapTime, endCapTime;
    int64 startProcessTime, endProcessTime;
    int64 st, et;
    int64 startOutputTime, endOutputTime;
    double timeOfIteration=40000.0;
    double timeOfCapture=20000.0, timeOfProcess=2000.0, timeOfOutput;
    // Main cycle
    for(;!end;){

        startCapTime = startIterationTime = cvGetTickCount();

        if(showPreview || showResult){
            char c;
            c=waitKey(1);
            if(!fullFrame.empty()) {
                switch (c) {
                case 'r':
                    bTrackerInitialized=false;
                    det.resetAim();
                    aim = det.getAim();
                    break;
                case 27:
                    end=true;
                    while(running) running=false;

                    break;
                default:
                    break;
                }
            }
        }

        if(!fullFrame.empty() ) { /// \todo 13.02.2017 Добавить мьютекс или семафор
            if(frameCounter>0)pzoom<<',';
        }
        else{
            clog << "Frame is empty" << endl;
            //                    end=true;
            continue;
        }
        startProcessTime = endCapTime = cvGetTickCount();


        ///// DETECTION AND TRACKING /////
        if(1){
            if(bTrackerInitialized && tracker != NULL && detWarning < detWarningLimit){
                if(bb !=NULL) {
                    delete bb; bb=NULL;
                    bb = new BBox();
                }
                bb = tracker->track(fullFrame); /// \todo 13.02.2017 Добавить мьютекс или семафор
                if(bb!=NULL){
                    st=cvGetTickCount();
                    aim = Rect(bb->x,
                               bb->y,
                               bb->width,
                               bb->height);
                    if(frameCounter%aimCheckerPer == 0){ /// \todo 14.02.2017 изменение размеров лица должно происходить и в меньшую сторону
                        det.detect(fullFrame((focus&Rect(0,0,fullShot.width,fullShot.height)))); /// \todo 13.02.2017 Добавить мьютекс или семафор
                        static unsigned int focusEx = focus.height*focusEx_;
                        if(det.foundFaces()){
                            if(detWarning>0){detWarning--;// -1 к предупреждению;
                                focus+=Point(focusEx,focusEx);
                                focus+=Size(-focusEx,-focusEx);
                            }
                            /// \todo 30.01.2017 надо подправить размер
                        }else{
                            if(detWarning<255){detWarning++;// +1 предупреждение
                                focus+=Point(-focusEx,-focusEx);
                                focus+=Size(focusEx,focusEx);
                            }
                        }
                        /// конкретные действия, что делать если мы теряем лицо
                        if(detWarning >= detWarningLimit){
                            bTrackerInitialized=false; // запускаем детекцию заново
                            det.resetAim();
                            focus=aim;
                        }
                    }
                    cam.update(aim); /// Сделать так, чтобы aim всегда держал нужные границы
                    focus=Rect(aim.x-(focus.width-aim.width)*0.5,aim.y-(focus.height-aim.height)*0.5,focus.width,focus.height);
                    et=cvGetTickCount();
                    clog << "some Time: " << (et-st)/cvGetTickFrequency() << endl;
                }
            }else{
                det.detect(fullFrame); /// \todo 13.02.2017 Добавить мьютекс или семафор
                if(det.aimDetected()){
                    aim = det.getAim();
                    focus=aim;
                    static unsigned int focusEx = focus.height*focusEx_;
                    focus+=Point(focusEx,focusEx);
                    focus+=Size(-focusEx,-focusEx);
                    if(tracker != NULL)delete tracker;
                    tracker = new ColorTracker();
                    tracker->init(fullFrame, /// \todo 13.02.2017 Добавить мьютекс или семафор
                                  aim.tl().x,
                                  aim.tl().y,
                                  aim.br().x,
                                  aim.br().y);
                    bTrackerInitialized = true;
                    detWarning=0;
                    cam.update(aim);
                }
            }
        }
        startOutputTime = endProcessTime = cvGetTickCount();
        try{
            /// \todo 18.05.2016 FileSaver
            if(recordResult || streamResult || showResult){
                resize(fullFrame(cam.getRoi()), *result , resultSize, 0,0, INTER_LINEAR ); /// \todo 13.02.2017 Добавить мьютекс или семафор
                if(recordResult){
                    outputVideo << *result;
                }
                if(streamResult){
                    //                            stream(result);
                    //                            clog<<"Writing result>>>>>>>>>>>>>"<< endl;
                    //                            Mat mat = result.clone();
                    //                            for(int col=0; col< mat.cols; ++col){
                    //                                for(int row=0;row,mat.rows;++row){
                    //                                            cout << (uchar)(col%255);
                    //                                            cout << (uchar)0;
                    //                                            cout << (uchar)0;
                    //                                }
                    //                            }
                }
                if(showResult){
                    imshow("Result",*result);
                }
            }
        }catch(Exception &mvEx){
            clog << "Result saving: "<< mvEx.msg << endl;
        }


        //    VIEW    //
        /// \todo 18.05.2016 class Drawer
        if(showPreview || streamPreview || recordPreview){ // Отрисовка области интереса
            pre.drawPreview(fullFrame,cam,bTrackerInitialized,focus,aim,det,detWarning,aimCheckerPer,frameCounter); /// \todo 13.02.2017 Добавить мьютекс или семафор
            if(showPreview)
                pre.show();
            if(streamPreview && !streamResult){
                //                        stream(pre.getPreview());
            }
            // Сохранение кадра
            if(recordPreview)
                previewVideo << pre.getPreview();
        } /// \todo 18.05.2016 end class Drawer


        // Сохранение статистики
        /// \todo 18.05.2016 class StatMan
        if(logFile.is_open()) {
            if(frameCounter<1){
                logFile << "frame\t";
                logFile << "det.getFacesFull().x, px"<<"\t"
                        <<"det.getFacesFull().y, px"<<"\t"
                       << "det.getFacesFull().width, px"<<"\t"
                       <<"det.getFacesFull().height, px"<<"\t"
                      << "faceProf.x, px"<<"\t"
                      <<"faceProf.y, px"<<"\t"
                     << "det.getFacesProf().width, px"<<"\t"
                     <<"det.getFacesProf().height, px"<<"\t"
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
            if(!det.getFacesFull().empty())
                logFile << det.getFacesFull()[0].x << "\t"
                                                   << det.getFacesFull()[0].y << "\t"
                                                   << det.getFacesFull()[0].width << "\t"
                                                   << det.getFacesFull()[0].height << "\t";
            else logFile << "\t\t\t\t";
            if(!det.getFacesProf().empty())
                logFile << det.getFacesProf()[0].x << "\t"
                                                   << det.getFacesProf()[0].y << "\t"
                                                   << det.getFacesProf()[0].width << "\t"
                                                   << det.getFacesProf()[0].height << "\t";
            else logFile << "\t\t\t\t";

            logFile << aim.x << "\t"
                    << aim.y << "\t"
                    << aim.width << "\t"
                    << aim.height << "\t";
            logFile << cam.getRoi().x << "\t"
                    << cam.getRoi().y << "\t"
                    << cam.getRoi().width << "\t"
                    << cam.getRoi().height << endl;
        }
        if(writeCropFile && cropFile.is_open()){
            cropFile << "zoompan=enable=eq(n\\,"<< frameCounter
                     << "):z=" << fullShot.width/roiFullSize.width
                     << ":x=" << roiFullSize.x
                     << ":y=" << roiFullSize.y << ":d=1";
        }

        clog <<"frame:"<< frameCounter
            << " ("<< (int)((100*(float)frameCounter)/(float)videoLength) <<"% of video)"
            << endl;

        endOutputTime = endIterationTime = cvGetTickCount();
        timeOfIteration = (endIterationTime - startIterationTime)/cvGetTickFrequency();
        timeOfCapture = (endCapTime - startCapTime)/cvGetTickFrequency();
        timeOfProcess = (endProcessTime - startProcessTime)/cvGetTickFrequency();
        timeOfOutput = (endOutputTime - startOutputTime)/cvGetTickFrequency();



        //            clog << "Time: " << timeOfIteration << " us\n";
        clog << "FrameRate: " << 0.04*1000000 / timeOfIteration<< " fps\n";
    }
    if(bb!=NULL){
        delete bb;
        bb = NULL;
    }
    if(tracker!=NULL){
        delete tracker;
    }
    if(logFile.is_open()){
        logFile.close(); /// \todo 18.05.2016 class StatMan
        clog << "The results have been written to " << "''"+outFileTitle+"''" << endl;
    }
    if(writeCropFile){ /// \todo 18.05.2016 class FileSaver
        //            fstream cropFile;
        //            cropFile.open(("../results/filter_"/*outFileTitle).c_str()*/,fstream::out);
        if(cropFile.is_open()){
            //                cropFile  << pzoom.str();
            cropFile.close();
        }else{
            clog << "Error with opening the file:" << "../results/test_"+outFileTitle+".crop" << endl;
        }
    }
    if(result!=NULL)delete result;
    cvDestroyAllWindows();

    cerr << kill(pidVlc,SIGKILL) << endl;
    resultThread.join();
    captureThread.join();
    return 0;
}
