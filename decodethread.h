#ifndef DECODETHREAD_H
#define DECODETHREAD_H

#include <QThread>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
}

typedef struct {
    int width;
    int height;
    int fps;
    AVPixelFormat fmt;
    const char *file;
} VideoDecodeSpec;

class DecodeThread : public QThread
{
    Q_OBJECT
private:
    void run() override;
public:
    DecodeThread(QObject *parent);
    ~DecodeThread();
signals:

};

#endif // DECODETHREAD_H
