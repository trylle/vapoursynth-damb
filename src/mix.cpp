#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <string>
#include <vector>
#include <memory>

#include <VapourSynth.h>
#include <VSHelper.h>

#include <cstdio>
#include <sndfile.h>

#include "shared.h"


template<class T>
struct VSDeleter;


template<>
struct VSDeleter<const VSFrameRef> {
    const VSAPI *vsapi = nullptr;

    VSDeleter(const VSAPI *vsapi_arg = nullptr)
            : vsapi(vsapi_arg)
    {

    }

    void operator()(const VSFrameRef *ptr) {
        vsapi->freeFrame(ptr);
    }
};


template<>
struct VSDeleter<VSNodeRef> {
    const VSAPI *vsapi = nullptr;

    VSDeleter(const VSAPI *vsapi_arg = nullptr)
            : vsapi(vsapi_arg)
    {

    }

    void operator()(VSNodeRef *ptr) {
        vsapi->freeNode(ptr);
    }
};


template<class T>
using VSUniquePtr = std::unique_ptr<T, VSDeleter<T>>;


typedef struct {
	VSUniquePtr<VSNodeRef> clipa;
    VSUniquePtr<VSNodeRef> clipb;
	double clipa_level = 1;
	double clipb_level = 1;
    const VSVideoInfo *vi = nullptr;

    //SF_INFO sfinfo = {};
    std::vector<uint8_t> buffer;
    //int sample_size = 0;
    //int sample_type = 0;
    //double samples_per_frame = 0;
    //double delay_seconds = 0;
    //sf_count_t delay_samples = 0;
} DambMixData;


static void VS_CC dambMixInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    DambMixData *d = (DambMixData *) * instanceData;
    vsapi->setVideoInfo(d->vi, 1, node);
}


template<class T>
void mix(std::vector<uint8_t> &dst_vec, const char *clipa_buffer, double clipa_level, const char *clipb_buffer, double clipb_level)
{
    auto dst = reinterpret_cast<T *>(dst_vec.data());
    auto srca = reinterpret_cast<const T *>(clipa_buffer);
    auto srcb = reinterpret_cast<const T *>(clipb_buffer);
    int samples = dst_vec.size()/sizeof(T);

    for (int i = 0; i<samples; ++i) {
        dst[i] = static_cast<T>(clipa_buffer[i]*clipa_level+clipb_buffer[i]*clipb_level);
    }
}


static const VSFrameRef *VS_CC dambMixGetFrame(int n, int activationReason, void **instanceData, void **frameData,
                                               VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    DambMixData *d = (DambMixData *) * instanceData;

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->clipa.get(), frameCtx);
        vsapi->requestFrameFilter(n, d->clipb.get(), frameCtx);
    } else if (activationReason == arAllFramesReady) {
        //const VSFrameRef *clipa_frame = vsapi->getFrameFilter(n, d->clipa, frameCtx);
        //const VSFrameRef *clipb_frame = vsapi->getFrameFilter(n, d->clipb, frameCtx);
        VSUniquePtr<const VSFrameRef> clipa_frame = { vsapi->getFrameFilter(n, d->clipa.get(), frameCtx), vsapi };
        VSUniquePtr<const VSFrameRef> clipb_frame = { vsapi->getFrameFilter(n, d->clipb.get(), frameCtx), vsapi };
        VSFrameRef *dst = vsapi->copyFrame(clipa_frame.get(), core);

        //vsapi->freeFrame(clipa_frame);
        //vsapi->freeFrame(clipb_frame);

        const VSMap *clipa_props = vsapi->getFramePropsRO(clipa_frame.get());
        const VSMap *clipb_props = vsapi->getFramePropsRO(clipb_frame.get());
        int err;
        int input_channels = vsapi->propGetInt(clipa_props, damb_channels, 0, &err);
        int input_samplerate = vsapi->propGetInt(clipa_props, damb_samplerate, 0, &err);
        int input_format = vsapi->propGetInt(clipa_props, damb_format, 0, &err);

        // TODO: error checking

        const char *clipa_buffer = vsapi->propGetData(clipa_props, damb_samples, 0, &err);
        sf_count_t clipa_buffer_size = vsapi->propGetDataSize(clipa_props, damb_samples, 0, &err);
        const char *clipb_buffer = vsapi->propGetData(clipb_props, damb_samples, 0, &err);
        sf_count_t clipb_buffer_size = vsapi->propGetDataSize(clipb_props, damb_samples, 0, &err);

        d->buffer.resize(clipa_buffer_size);

        auto sample_type = getSampleType(input_format);

        if (sample_type == SF_FORMAT_PCM_16)
            mix<short>(d->buffer, clipa_buffer, d->clipa_level, clipb_buffer, d->clipb_level);
        else if (sample_type == SF_FORMAT_PCM_32)
            mix<int>(d->buffer, clipa_buffer, d->clipa_level, clipb_buffer, d->clipb_level);
        else if (sample_type == SF_FORMAT_FLOAT)
            mix<float>(d->buffer, clipa_buffer, d->clipa_level, clipb_buffer, d->clipb_level);
        else
            mix<double>(d->buffer, clipa_buffer, d->clipa_level, clipb_buffer, d->clipb_level);

        VSMap *props = vsapi->getFramePropsRW(dst);
        vsapi->propSetData(props, damb_samples, (char *)d->buffer.data(), d->buffer.size(), paReplace);
        vsapi->propSetInt(props, damb_channels, input_channels, paReplace);
        vsapi->propSetInt(props, damb_samplerate, input_samplerate, paReplace);
        vsapi->propSetInt(props, damb_format, input_format, paReplace);

        return dst;
    }

    return nullptr;
}


static void VS_CC dambMixFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    DambMixData *d = (DambMixData *)instanceData;

    delete d;
}


static void VS_CC dambMixCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    DambMixData d;
    DambMixData *data;
    int err;

    d.clipa = { vsapi->propGetNode(in, "clipa", 0, NULL), vsapi };
    d.clipb = { vsapi->propGetNode(in, "clipb", 0, NULL), vsapi };
    d.clipa_level = vsapi->propGetFloat(in, "levela", 0, NULL);
    d.clipb_level = vsapi->propGetFloat(in, "levelb", 0, NULL);
    d.vi = vsapi->getVideoInfo(d.clipa.get());

    if (!d.vi->numFrames) {
        vsapi->setError(out, "Read: Can't accept clips with unknown length.");
        return;
    }

    if (!d.vi->fpsNum || !d.vi->fpsDen) {
        vsapi->setError(out, "Read: Can't accept clips with variable frame rate.");
        return;
    }

    /*d.sfinfo.format = 0;
    d.sndfile = sf_open(d.filename.c_str(), SFM_READ, &d.sfinfo);
    if (d.sndfile == NULL) {
        vsapi->setError(out, std::string("Read: Couldn't open audio file. Error message from libsndfile: ").append(sf_strerror(NULL)).c_str());
        vsapi->freeNode(d.node);
        return;
    }

    if (!isAcceptableFormatType(d.sfinfo.format)) {
        vsapi->setError(out, "Read: Audio file's type is not supported.");
        sf_close(d.sndfile);
        vsapi->freeNode(d.node);
        return;
    }

    if (!isAcceptableFormatSubtype(d.sfinfo.format)) {
        vsapi->setError(out, "Read: Audio file's subtype is not supported.");
        sf_close(d.sndfile);
        vsapi->freeNode(d.node);
        return;
    }

    d.samples_per_frame = (d.sfinfo.samplerate * d.vi->fpsDen) / (double)d.vi->fpsNum;

    d.sample_type = getSampleType(d.sfinfo.format);
    d.sample_size = getSampleSize(d.sample_type);

    // sample_count will be sometimes (int)samples_per_frame,
    // sometimes (int)(samples_per_frame + 1), depending on the frame number
    d.buffer = (uint8_t *)malloc((int)(d.samples_per_frame + 1) * d.sfinfo.channels * d.sample_size);

    d.delay_samples = (sf_count_t)(d.delay_seconds * d.sfinfo.samplerate);*/

    data = new DambMixData();
    *data = std::move(d);

	vsapi->createFilter(in, out, "Mix", dambMixInit, dambMixGetFrame, dambMixFree, fmSerial, 0, data, core);
}


void mixRegister(VSRegisterFunction registerFunc, VSPlugin *plugin) {
    registerFunc("Mix",
            "clipa:clip;"
            "clipb:clip;"
            "levela:float:opt;"
            "levelb:float:opt;"
            , dambMixCreate, 0, plugin);
}
