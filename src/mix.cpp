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

    std::vector<uint8_t> buffer;
} DambMixData;


static void VS_CC dambMixInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    DambMixData *d = (DambMixData *) * instanceData;
    vsapi->setVideoInfo(d->vi, 1, node);
}


template<class T>
void mix(uint8_t *u8_dst_begin, uint8_t *u8_dst_end, const char *clipa_buffer, double clipa_level, const char *clipb_buffer_begin, const char *clipb_buffer_end, double clipb_level, int input_channels)
{
    auto dst_begin = reinterpret_cast<T *>(u8_dst_begin);
    auto dst_end = reinterpret_cast<T *>(u8_dst_end);
    auto srca = reinterpret_cast<const T *>(clipa_buffer);
    auto srcb_begin = reinterpret_cast<const T *>(clipb_buffer_begin);
    auto srcb_end = reinterpret_cast<const T *>(clipb_buffer_end);
    int samples = (dst_end-dst_begin)/input_channels;
    int clipb_samples = (srcb_end-srcb_begin)/input_channels;
    auto max_sample = samples-1;
    auto max_clipb_sample = clipb_samples-1;

    for (int i = 0; i<samples; ++i) {
        for (int k = 0; k<input_channels; ++k) {
            auto j = i*max_clipb_sample/max_sample;
            auto lambda = ((i*max_clipb_sample)%max_sample)/double(max_sample);

            if (j==max_clipb_sample) {
                --j;
                lambda=1;
            }

            auto srcb_value = srcb_begin[j*input_channels+k]+(srcb_begin[(j+1)*input_channels+k]-srcb_begin[j*input_channels+k])*lambda;

            dst_begin[i*input_channels+k] = static_cast<T>(srca[i*input_channels+k]*clipa_level+srcb_value*clipb_level);
        }
    }
}


static const VSFrameRef *VS_CC dambMixGetFrame(int n, int activationReason, void **instanceData, void **frameData,
                                               VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    DambMixData *d = (DambMixData *) * instanceData;

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->clipa.get(), frameCtx);
        vsapi->requestFrameFilter(n, d->clipb.get(), frameCtx);
    } else if (activationReason == arAllFramesReady) {
        VSUniquePtr<const VSFrameRef> clipa_frame = { vsapi->getFrameFilter(n, d->clipa.get(), frameCtx), vsapi };
        VSUniquePtr<const VSFrameRef> clipb_frame = { vsapi->getFrameFilter(n, d->clipb.get(), frameCtx), vsapi };
        VSFrameRef *dst = vsapi->copyFrame(clipa_frame.get(), core);

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

        auto mix_helper=[&] (std::uint8_t *dst_begin, std::uint8_t *dst_end, const char *clipa_buffer, const char *clipb_buffer_begin, const char *clipb_buffer_end)
        {
            if (sample_type == SF_FORMAT_PCM_16)
                mix<short>(dst_begin, dst_end, clipa_buffer, d->clipa_level, clipb_buffer_begin, clipb_buffer_end, d->clipb_level, input_channels);
            else if (sample_type == SF_FORMAT_PCM_32)
                mix<int>(dst_begin, dst_end, clipa_buffer, d->clipa_level, clipb_buffer_begin, clipb_buffer_end, d->clipb_level, input_channels);
            else if (sample_type == SF_FORMAT_FLOAT)
                mix<float>(dst_begin, dst_end, clipa_buffer, d->clipa_level, clipb_buffer_begin, clipb_buffer_end, d->clipb_level, input_channels);
            else
                mix<double>(dst_begin, dst_end, clipa_buffer, d->clipa_level, clipb_buffer_begin, clipb_buffer_end, d->clipb_level, input_channels);        
        };

        mix_helper(d->buffer.data(), d->buffer.data()+d->buffer.size(), clipa_buffer, clipb_buffer, clipb_buffer+clipb_buffer_size);

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

    auto levela = vsapi->propGetFloat(in, "levela", 0, &err);

    if (!err)
        d.clipa_level = levela;

    auto levelb = vsapi->propGetFloat(in, "levelb", 0, &err);

    if (!err)
        d.clipb_level = levelb;

    d.vi = vsapi->getVideoInfo(d.clipa.get());

    if (!d.vi->numFrames) {
        vsapi->setError(out, "Read: Can't accept clips with unknown length.");
        return;
    }

    if (!d.vi->fpsNum || !d.vi->fpsDen) {
        vsapi->setError(out, "Read: Can't accept clips with variable frame rate.");
        return;
    }

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
