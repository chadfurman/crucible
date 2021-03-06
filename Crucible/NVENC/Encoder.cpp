#include <obs.hpp>
#include <obs-encoder.h>
#include <obs-avc.h>
#include <util/profiler.hpp>

#include "dynlink_cuda.h"
#include "nvEncodeAPI.h"

#include "../scopeguard.hpp"

#include <array>
#include <deque>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "../ThreadTools.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <d3d11.h>
#include <util/windows/ComPtr.hpp>
#include <util/platform.h>

using namespace std;


#define do_log(level, encoder, format, ...) \
	blog(level, "[NVENC/Encoder: '%s'] " format, \
			obs_encoder_get_name(encoder), ##__VA_ARGS__)

#define error(format, ...) do_log(LOG_ERROR,   enc->encoder, format, ##__VA_ARGS__)
#define warn(format, ...)  do_log(LOG_WARNING, enc->encoder, format, ##__VA_ARGS__)
#define info(format, ...)  do_log(LOG_INFO,    enc->encoder, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG,   enc->encoder, format, ##__VA_ARGS__)

#if defined(_WIN32) || defined(__CYGWIN__)
# define CUDA_LIBNAME "nvcuda.dll"
# if ARCH_X86_64
#  define NVENC_LIBNAME "nvEncodeAPI64.dll"
# else
#  define NVENC_LIBNAME "nvEncodeAPI.dll"
# endif
#endif

namespace {
	struct LibUnloader {
		void operator()(HMODULE mod) { FreeLibrary(mod); }
	};
	using Library = unique_ptr<remove_pointer_t<HMODULE>, LibUnloader>;

#define LOAD_LIB(name) \
		lib.reset(LoadLibraryA(name)); \
		if (!lib) { \
			blog(LOG_WARNING, "NVENC/Encoder: failed to load library " name); \
			return false; \
		}

#define LOAD_FN(x) \
		x = reinterpret_cast<decltype(x)>(GetProcAddress(lib.get(), #x)); \
		if (!x)  { \
			blog(LOG_WARNING, "NVENC/Encoder: failed to load function " #x); \
			return false; \
		}

	struct CUDAFunctions {
		cuInit_t *cuInit = nullptr;
		cuDeviceGetCount_t *cuDeviceGetCount = nullptr;
		cuDeviceGet_t *cuDeviceGet = nullptr;
		cuD3D11GetDevice_t *cuD3D11GetDevice = nullptr;
		cuDeviceGetName_t *cuDeviceGetName = nullptr;
		cuDeviceComputeCapability_t *cuDeviceComputeCapability = nullptr;
		cuCtxCreate_v2_t *cuCtxCreate = nullptr;
		cuCtxPushCurrent_v2_t *cuCtxPushCurrent = nullptr;
		cuCtxPopCurrent_v2_t *cuCtxPopCurrent = nullptr;
		cuCtxDestroy_v2_t *cuCtxDestroy = nullptr;
		cuMemAlloc_v2_t *cuMemAlloc = nullptr;
		cuMemAllocPitch_t *cuMemAllocPitch = nullptr;
		cuMemFree_v2_t *cuMemFree = nullptr;
		cuMemcpy2D_v2_t *cuMemcpy2D = nullptr;
		cuGetErrorName_t *cuGetErrorName = nullptr;
		cuGetErrorString_t *cuGetErrorString = nullptr;
		cuStreamSynchronize_t *cuStreamSynchronize = nullptr;

		cuGraphicsD3D11RegisterResource_t *cuGraphicsD3D11RegisterResource = nullptr;
		cuGraphicsMapResources_t *cuGraphicsMapResources = nullptr;
		cuGraphicsResourceGetMappedPointer_t *cuGraphicsResourceGetMappedPointer = nullptr;
		cuGraphicsSubResourceGetMappedArray_t *cuGraphicsSubResourceGetMappedArray = nullptr;
		cuGraphicsUnmapResources_t *cuGraphicsUnmapResources = nullptr;
		cuGraphicsUnregisterResource_t *cuGraphicsUnregisterResource = nullptr;

		Library lib;

		bool Load()
		{
			LOAD_LIB(CUDA_LIBNAME);

			auto free_on_error = guard([&]
			{
				lib.reset();
			});

			LOAD_FN(cuInit);
			LOAD_FN(cuDeviceGetCount);
			LOAD_FN(cuDeviceGet);
			LOAD_FN(cuD3D11GetDevice);
			LOAD_FN(cuDeviceGetName);
			LOAD_FN(cuDeviceComputeCapability);
			LOAD_FN(cuCtxCreate);
			LOAD_FN(cuCtxPushCurrent);
			LOAD_FN(cuCtxPopCurrent);
			LOAD_FN(cuCtxDestroy);
			LOAD_FN(cuMemAlloc);
			LOAD_FN(cuMemAllocPitch);
			LOAD_FN(cuMemFree);
			LOAD_FN(cuMemcpy2D);
			LOAD_FN(cuGetErrorName);
			LOAD_FN(cuGetErrorString);
			LOAD_FN(cuStreamSynchronize);

			LOAD_FN(cuGraphicsD3D11RegisterResource);
			LOAD_FN(cuGraphicsMapResources);
			LOAD_FN(cuGraphicsResourceGetMappedPointer);
			LOAD_FN(cuGraphicsSubResourceGetMappedArray);
			LOAD_FN(cuGraphicsUnmapResources);
			LOAD_FN(cuGraphicsUnregisterResource);

			free_on_error.dismiss();

			return true;
		}
	};

	struct NVENCFunctions {
		typedef NVENCSTATUS NVENCAPI NvEncodeAPICreateInstance_t(NV_ENCODE_API_FUNCTION_LIST *functionList);
		typedef NVENCSTATUS NVENCAPI NvEncodeAPIGetMaxSupportedVersion_t(uint32_t* version);

		NvEncodeAPICreateInstance_t *NvEncodeAPICreateInstance = nullptr;
		NvEncodeAPIGetMaxSupportedVersion_t *NvEncodeAPIGetMaxSupportedVersion = nullptr;

		Library lib;

		bool Load()
		{
			LOAD_LIB(NVENC_LIBNAME);

			auto free_on_error = guard([&]
			{
				lib.reset();
			});

			LOAD_FN(NvEncodeAPICreateInstance);
			LOAD_FN(NvEncodeAPIGetMaxSupportedVersion);

			free_on_error.dismiss();

			return true;
		}
	};

#undef LOAD_LIB
#undef LOAD_FN

	struct CUDAResult {
		shared_ptr<CUDAFunctions> funcs;
		CUresult res = CUDA_SUCCESS;
		CUDAResult(shared_ptr<CUDAFunctions> funcs) : funcs(funcs) {}

		CUDAResult &operator=(CUresult res_)
		{
			res = res_;
			return *this;
		}

		explicit operator bool() { return res != CUDA_SUCCESS; }

		const char *Name()
		{
			const char *str = nullptr;
			if (!funcs || funcs->cuGetErrorName(res, &str) != CUDA_SUCCESS || !str)
				return "CUDA_ERROR_INVALID_VALUE";
			return str;
		}

		const char *Description()
		{
			const char *str = nullptr;
			if (!funcs || funcs->cuGetErrorString(res, &str) != CUDA_SUCCESS || !str)
				return "Unknown error";
			return str;
		}
	};

	enum struct H264Profile {
		Baseline,
		Main,
		High
	};

	using CUDAContext = shared_ptr<remove_pointer_t<CUcontext>>;

	struct Surface {
		NV_ENC_INPUT_PTR input = nullptr;

		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t pitch = 0;

		NV_ENC_OUTPUT_PTR output = nullptr;
		NV_ENC_BUFFER_FORMAT format = NV_ENC_BUFFER_FORMAT_NV12_PL;

		unique_ptr<void, HandleDeleter> event;

		CUdeviceptr cuda_memory = 0;
		size_t cuda_pitch = 0;
		size_t copy_width = 0;
		size_t copy_height = 0;
		void *registered_cuda_memory = nullptr;
	};

	struct Encoder {
		obs_encoder_t *encoder = nullptr;

		shared_ptr<CUDAFunctions> cuda;
		shared_ptr<NVENCFunctions> nvenc;
		NV_ENCODE_API_FUNCTION_LIST funcs = { 0 };

		CUDAContext ctx;

		struct cuda_input_tex {
			uint32_t shared_handle;
			ComPtr<ID3D11Texture2D> d3d11_tex;
			CUgraphicsResource cuda_tex;
		};

		map<gs_texture_t*, cuda_input_tex> input_textures;

		ComPtr<ID3D11Device> d3d11_device;


		bool dynamic_bitrate = false;
		uint32_t b_frames = 0;
		uint32_t b_frames_actual = 0;
		bool b_frames_strict = false;
		double keyint_sec = 5.;

		uint32_t bitrate = 0;
		NV_ENC_PARAMS_RC_MODE rc_mode = NV_ENC_PARAMS_RC_VBR_HQ;

		H264Profile profile;


		void *nv_encoder = nullptr;

		NV_ENC_CONFIG encode_config = { 0 };
		NV_ENC_INITIALIZE_PARAMS init_params = { 0 };

		bool allow_rgba_texture_input = false;
		bool allow_texture_input = true;
		bool use_texture_input = false;
		video_format input_format = VIDEO_FORMAT_NV12;

		vector<Surface> surfaces;

		deque<Surface*> idle;
		deque<Surface*> processing;
		deque<Surface*> ready;

		vector<uint8_t> headers;

		deque<uint64_t> timestamps;
		bool b_frame_pts_calculated = false;

		vector<uint32_t> slice_offsets;
		vector<uint8_t> slice_data;

		Encoder(obs_encoder_t *encoder) : encoder(encoder)
		{
			encode_config.version = NV_ENC_CONFIG_VER;
			encode_config.rcParams.version = NV_ENC_RC_PARAMS_VER;

			init_params.version = NV_ENC_INITIALIZE_PARAMS_VER;
			init_params.encodeGUID = NV_ENC_CODEC_H264_GUID;
			init_params.presetGUID = NV_ENC_PRESET_DEFAULT_GUID;

			init_params.encodeWidth = obs_encoder_get_width(encoder);
			init_params.encodeHeight = obs_encoder_get_height(encoder);

			init_params.darWidth = init_params.encodeWidth;
			init_params.darHeight = init_params.encodeHeight;

			init_params.enablePTD = 1;

			init_params.encodeConfig = &encode_config;

			auto video = obs_encoder_video(encoder);
			if (!video)
				throw make_pair("obs_encoder_video returned nullptr", __LINE__);

			auto voi = video_output_get_info(video);
			if (!voi)
				throw make_pair("video_output_get_info returned nullptr", __LINE__);

			init_params.frameRateNum = voi->fps_num;
			init_params.frameRateDen = voi->fps_den;
		}

		~Encoder()
		{
			Release();
		}

		void Release()
		{
			if (!nvenc)
				return;

			if (nv_encoder) {
				CUDAResult res(cuda);
				if (res = cuda->cuCtxPushCurrent(ctx.get())) {
					do_log(LOG_ERROR, encoder, "~Encoder: cuCtxPushCurrent returned %s (%d): %s", res.Name(), res.res, res.Description());
					return;
				}

				auto pop_context_impl = [&]
				{
					CUcontext dummy;
					if (res = cuda->cuCtxPopCurrent(&dummy)) {
						do_log(LOG_ERROR, encoder, "~Encoder: cuCtxPopCurrent returned %s (%d): %s", res.Name(), res.res, res.Description());
						return false;
					}

					return true;
				};

				auto context_guard = guard(pop_context_impl);

				NV_ENC_PIC_PARAMS pic = { 0 };
				pic.version = NV_ENC_PIC_PARAMS_VER;
				pic.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
				funcs.nvEncEncodePicture(nv_encoder, &pic);

				for (auto &shared_tex : input_textures) {
					cuda->cuGraphicsUnmapResources(1, &shared_tex.second.cuda_tex, 0);
					cuda->cuGraphicsUnregisterResource(shared_tex.second.cuda_tex);
				}
				input_textures.clear();

				d3d11_device.Release();

				for (auto &surface : surfaces) {
					if (funcs.nvEncUnregisterAsyncEvent && surface.event) {
						NV_ENC_EVENT_PARAMS evt_params{};
						evt_params.version = NV_ENC_EVENT_PARAMS_VER;
						evt_params.completionEvent = surface.event.get();
						funcs.nvEncUnregisterAsyncEvent(nv_encoder, &evt_params);
					}

					if (surface.cuda_memory) {
						if (funcs.nvEncUnmapInputResource && surface.input)
							funcs.nvEncUnmapInputResource(nv_encoder, surface.input);
						if (funcs.nvEncUnregisterResource && surface.registered_cuda_memory)
							funcs.nvEncUnregisterResource(nv_encoder, surface.registered_cuda_memory);
						if (surface.cuda_memory)
							cuda->cuMemFree(surface.cuda_memory);
					} else {
						if (funcs.nvEncDestroyInputBuffer && surface.input)
							funcs.nvEncDestroyInputBuffer(nv_encoder, surface.input);
					}

					if (funcs.nvEncDestroyBitstreamBuffer && surface.output)
						funcs.nvEncDestroyBitstreamBuffer(nv_encoder, surface.output);
				}

				if (funcs.nvEncDestroyEncoder)
					funcs.nvEncDestroyEncoder(nv_encoder);
			}

			surfaces.clear();
		}
	};

	static Encoder *cast(void *context)
	{
		return reinterpret_cast<Encoder*>(context);
	}

#define STRINGIFY_VAL(x) case x: return #x;

	struct NVENCStatus {
		NVENCSTATUS sts = NV_ENC_SUCCESS;
		NVENCStatus() = default;
		NVENCStatus(NVENCSTATUS sts) : sts(sts) {}

		explicit operator bool() { return sts != NV_ENC_SUCCESS; }

		const char *Name()
		{
			switch (sts) {
			STRINGIFY_VAL(NV_ENC_SUCCESS);
			STRINGIFY_VAL(NV_ENC_ERR_NO_ENCODE_DEVICE);
			STRINGIFY_VAL(NV_ENC_ERR_UNSUPPORTED_DEVICE);
			STRINGIFY_VAL(NV_ENC_ERR_INVALID_ENCODERDEVICE);
			STRINGIFY_VAL(NV_ENC_ERR_INVALID_DEVICE);
			STRINGIFY_VAL(NV_ENC_ERR_DEVICE_NOT_EXIST);
			STRINGIFY_VAL(NV_ENC_ERR_INVALID_PTR);
			STRINGIFY_VAL(NV_ENC_ERR_INVALID_EVENT);
			STRINGIFY_VAL(NV_ENC_ERR_INVALID_PARAM);
			STRINGIFY_VAL(NV_ENC_ERR_INVALID_CALL);
			STRINGIFY_VAL(NV_ENC_ERR_OUT_OF_MEMORY);
			STRINGIFY_VAL(NV_ENC_ERR_ENCODER_NOT_INITIALIZED);
			STRINGIFY_VAL(NV_ENC_ERR_UNSUPPORTED_PARAM);
			STRINGIFY_VAL(NV_ENC_ERR_LOCK_BUSY);
			STRINGIFY_VAL(NV_ENC_ERR_NOT_ENOUGH_BUFFER);
			STRINGIFY_VAL(NV_ENC_ERR_INVALID_VERSION);
			STRINGIFY_VAL(NV_ENC_ERR_MAP_FAILED);
			STRINGIFY_VAL(NV_ENC_ERR_NEED_MORE_INPUT);
			STRINGIFY_VAL(NV_ENC_ERR_ENCODER_BUSY);
			STRINGIFY_VAL(NV_ENC_ERR_EVENT_NOT_REGISTERD);
			STRINGIFY_VAL(NV_ENC_ERR_GENERIC);
			STRINGIFY_VAL(NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY);
			STRINGIFY_VAL(NV_ENC_ERR_UNIMPLEMENTED);
			STRINGIFY_VAL(NV_ENC_ERR_RESOURCE_REGISTER_FAILED);
			STRINGIFY_VAL(NV_ENC_ERR_RESOURCE_NOT_REGISTERED);
			STRINGIFY_VAL(NV_ENC_ERR_RESOURCE_NOT_MAPPED);
			}

			return "Unknown NVENC_STATUS";
		}

		void Error(Encoder *enc, const char *func)
		{
			error("%s returned %#x (%s)", func, sts, Name());
		}

		void Warn(Encoder *enc, const char *func)
		{
			warn("%s returned %#x (%s)", func, sts, Name());
		}
	};

	struct NVENCCaps {
		NV_ENC_CAPS cap;
		NVENCCaps(NV_ENC_CAPS cap) : cap(cap) {}

		const char *Name()
		{
			switch (cap) {
			STRINGIFY_VAL(NV_ENC_CAPS_NUM_MAX_BFRAMES);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_FIELD_ENCODING);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_MONOCHROME);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_FMO);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_QPELMV);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_BDIRECT_MODE);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_CABAC);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_ADAPTIVE_TRANSFORM);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_RESERVED);
			STRINGIFY_VAL(NV_ENC_CAPS_NUM_MAX_TEMPORAL_LAYERS);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_HIERARCHICAL_PFRAMES);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_HIERARCHICAL_BFRAMES);
			STRINGIFY_VAL(NV_ENC_CAPS_LEVEL_MAX);
			STRINGIFY_VAL(NV_ENC_CAPS_LEVEL_MIN);
			STRINGIFY_VAL(NV_ENC_CAPS_SEPARATE_COLOUR_PLANE);
			STRINGIFY_VAL(NV_ENC_CAPS_WIDTH_MAX);
			STRINGIFY_VAL(NV_ENC_CAPS_HEIGHT_MAX);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_TEMPORAL_SVC);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_DYN_RES_CHANGE);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_DYN_FORCE_CONSTQP);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_DYN_RCMODE_CHANGE);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_SUBFRAME_READBACK);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_CONSTRAINED_ENCODING);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_INTRA_REFRESH);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_DYNAMIC_SLICE_MODE);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_REF_PIC_INVALIDATION);
			STRINGIFY_VAL(NV_ENC_CAPS_PREPROC_SUPPORT);
			STRINGIFY_VAL(NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT);
			STRINGIFY_VAL(NV_ENC_CAPS_MB_NUM_MAX);
			STRINGIFY_VAL(NV_ENC_CAPS_MB_PER_SEC_MAX);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_YUV444_ENCODE);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_LOSSLESS_ENCODE);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_SAO);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_MEONLY_MODE);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_LOOKAHEAD);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_10BIT_ENCODE);
			STRINGIFY_VAL(NV_ENC_CAPS_NUM_MAX_LTR_FRAMES);
			STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_WEIGHTED_PREDICTION);
			STRINGIFY_VAL(NV_ENC_CAPS_EXPOSED_COUNT);
			}

			return "Unknown NV_ENC_CAPS value";
		}
	};
}

static void EncoderDefaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "bitrate", 2500);
	obs_data_set_default_double(settings, "keyint_sec", 0);
	obs_data_set_default_string(settings, "preset", "default");
	obs_data_set_default_string(settings, "profile", "high");
	obs_data_set_default_string(settings, "rate_control", "vbr");
	obs_data_set_default_int(settings, "bf", 3);
	obs_data_set_default_bool(settings, "bf_strict", false);
	obs_data_set_default_bool(settings, "dynamic_bitrate", false);
}

static struct {
	string name;
	GUID guid;
} presets[] = {
#define ADD_PRESET(x) {#x, NV_ENC_PRESET_## x ##_GUID}
	ADD_PRESET(DEFAULT),
	ADD_PRESET(HP),
	ADD_PRESET(HQ),
	ADD_PRESET(BD),
	ADD_PRESET(LOW_LATENCY_DEFAULT),
	ADD_PRESET(LOW_LATENCY_HQ),
	ADD_PRESET(LOW_LATENCY_HP),
	ADD_PRESET(LOSSLESS_DEFAULT),
	ADD_PRESET(LOSSLESS_HP),
};

static void LoadConfig(Encoder *enc, obs_data_t *settings)
{
	using boost::iequals;

	enc->dynamic_bitrate = obs_data_get_bool(settings, "dynamic_bitrate");
	enc->b_frames = static_cast<uint32_t>(obs_data_get_int(settings, "bf"));
	enc->b_frames_actual = enc->b_frames;
	enc->b_frames_strict = obs_data_get_bool(settings, "bf_strict");

	enc->keyint_sec = obs_data_get_double(settings, "keyint_sec");

	enc->bitrate = static_cast<uint32_t>(obs_data_get_int(settings, "bitrate"));

	auto profile = obs_data_get_string(settings, "profile");
	if (iequals(profile, "baseline"))
		enc->profile = H264Profile::Baseline;
	else if (iequals(profile, "main"))
		enc->profile = H264Profile::Main;
	else
		enc->profile = H264Profile::High;

	auto rc = obs_data_get_string(settings, "rate_control");
	if (iequals(rc, "cbr")) {
		enc->rc_mode = NV_ENC_PARAMS_RC_CBR_LOWDELAY_HQ;
	} else /*if (iequals(rc, "vbr"))*/ {
		enc->rc_mode = NV_ENC_PARAMS_RC_VBR_HQ;
	}

	auto preset = obs_data_get_string(settings, "preset");
	auto it = find_if(begin(presets), end(presets), [&](const decltype(presets[0]) &val)
	{
		return iequals(preset, val.name);
	});
	if (it != end(presets))
		enc->init_params.presetGUID = it->guid;
}

static uint32_t MakeVersion(uint32_t major, uint32_t minor)
{
	return major << 4 | minor;
}

static bool LoadFunctions(Encoder *enc)
{
	CUDAFunctions cuda;
	if (!cuda.Load())
		return false;

	NVENCFunctions nvenc;
	if (!nvenc.Load())
		return false;

	uint32_t max_version;
	NVENCStatus sts;
	if (sts = nvenc.NvEncodeAPIGetMaxSupportedVersion(&max_version)) {
		warn("NvEncodeAPIGetMaxSupportedVersion returned %#x (%s)", sts.sts, sts.Name());
		return false;
	}

	info("Loaded NVENC version %d.%d", max_version >> 4, max_version & 0xf);

	if (MakeVersion(NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION) > max_version) {
		warn("Driver does not support required version:\n\tRequired: %d.%d\n\tFound: %d.%d",
			NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION, max_version >> 4, max_version & 0xf);
		return false;
	}

	NV_ENCODE_API_FUNCTION_LIST funcs = { 0 };
	funcs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
	if (sts = nvenc.NvEncodeAPICreateInstance(&funcs)) {
		warn("NvEncodeAPICreateInstance returned %#x (%s)", sts.sts, sts.Name());
		return false;
	}

	enc->cuda = make_shared<CUDAFunctions>(move(cuda));
	enc->nvenc = make_shared<NVENCFunctions>(move(nvenc));
	enc->funcs = funcs;

	return true;
}

static bool OpenSession(Encoder *enc)
{
	NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params = { 0 };
	params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
	params.apiVersion = NVENCAPI_VERSION;
	params.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
	params.device = enc->ctx.get();

	void *encoder;
	if (NVENCStatus sts = enc->funcs.nvEncOpenEncodeSessionEx(&params, &encoder)) {
		warn("nvEncOpenEncodeSessionEx returned %#x (%s)", sts.sts, sts.Name());
		return false;
	}

	enc->nv_encoder = encoder;
	return true;
}

static bool CheckCodec(Encoder *enc)
{
	NVENCStatus sts;

	uint32_t count = 0;
	if (sts = enc->funcs.nvEncGetEncodeGUIDCount(enc->nv_encoder, &count)) {
		warn("nvEncGetEncodeGUIDCount returned %#x (%s)", sts.sts, sts.Name());
		return false;
	}

	if (!count) {
		warn("nvEncGetEncodeGUIDCount found 0 GUIDs");
		return false;
	}

	vector<GUID> guids(count);
	if (sts = enc->funcs.nvEncGetEncodeGUIDs(enc->nv_encoder, guids.data(), guids.size(), &count)) {
		warn("nvEncGetEncodeGUIDs returned %#x (%s)", sts.sts, sts.Name());
		return false;
	}

	auto it = find(begin(guids), end(guids), enc->init_params.encodeGUID);
	if (it == end(guids)) {
		warn("encoder does not support H264");
		return false;
	}

	return true;
}

static bool CheckCapabilities(Encoder *enc, bool allow_async)
{
	NV_ENC_CAPS_PARAM params = { 0 };
	params.version = NV_ENC_CAPS_PARAM_VER;
	int val;

	auto check_cap = [&](NVENCCaps cap, auto &&validate)
	{
		params.capsToQuery = cap.cap;
		if (NVENCStatus sts = enc->funcs.nvEncGetEncodeCaps(enc->nv_encoder, enc->init_params.encodeGUID, &params, &val)) {
			warn("nvEncGetEncodeCaps returned %#x (%s) when checking %#x (%s)",
				sts.sts, sts.Name(), cap.cap, cap.Name());
			return false;
		}

		return validate();
	};


	if (!check_cap(NV_ENC_CAPS_WIDTH_MAX, [&]
	{
		auto width = obs_encoder_get_width(enc->encoder);
		if (val >= 0 && static_cast<uint32_t>(val) >= width)
			return true;

		warn("width %d not supported (maximum is %d)", width, val);
		return false;
	}))
		return false;

	if (!check_cap(NV_ENC_CAPS_HEIGHT_MAX, [&]
	{
		auto height = obs_encoder_get_height(enc->encoder);
		if (val >= 0 && static_cast<uint32_t>(val) >= height)
			return true;

		warn("height %d not supported (maximum is %d)", height, val);
		return false;
	}))
		return false;

	if (enc->dynamic_bitrate && !check_cap(NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE, [&]
	{
		if (val)
			return true;

		warn("encoder does not support dynamic bitrate change");
		return false;
	}))
		return false;

	if (enc->b_frames && !check_cap(NV_ENC_CAPS_NUM_MAX_BFRAMES, [&]
	{
		if (val >= 0 && static_cast<uint32_t>(val) >= enc->b_frames)
			return true;

		if (!enc->b_frames_strict) {
			info("%d b-frames requested, but max is %d, using max", enc->b_frames, val);
			enc->b_frames_actual = val;
			return true;
		}

		warn("%d b-frames requested, but max is %d", enc->b_frames, val);
		return false;
	}))
		return false;

	if (allow_async) {
		check_cap(NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT, [&]
		{
			info("ASYNC_ENCODE %ssupported", val ? "" : "not ");
			enc->init_params.enableEncodeAsync = val;
			return true;
		});
	}

	if (enc->allow_texture_input) {
		uint32_t count;
		if (NVENCStatus sts = enc->funcs.nvEncGetInputFormatCount(enc->nv_encoder, enc->init_params.encodeGUID, &count)) {
			sts.Warn(enc, "nvEncGetInputFormatCount");
			return false;
		}

		vector<NV_ENC_BUFFER_FORMAT> formats;
		formats.resize(count);
		if (NVENCStatus sts = enc->funcs.nvEncGetInputFormats(enc->nv_encoder, enc->init_params.encodeGUID, formats.data(), count, &count)) {
			sts.Warn(enc, "nvEncGetInputFormats");
			return false;
		}

		formats.resize(count);
		enc->allow_rgba_texture_input = find(begin(formats), end(formats), NV_ENC_BUFFER_FORMAT_ABGR) != end(formats);
	}

	return true;
}

static bool ActualOpenDevice(Encoder *enc, CUdevice device, bool allow_async, int idx)
{
	CUDAResult res(enc->cuda);

	auto cuda_error = [&](const char *func)
	{
		warn("%s returned %s (%#x): %s", func, res.Name(), res.res, res.Description());
		return false;
	};

	auto make_version = [](int major, int minor) { return major << 4 | minor; };

	array<char, 128> name;
	if (res = enc->cuda->cuDeviceGetName(name.data(), name.size(), device))
		return cuda_error("cuDeviceGetName");

	int major, minor;
	if (res = enc->cuda->cuDeviceComputeCapability(&major, &minor, device))
		return cuda_error("cuDeviceComputeCapability");

	auto log_gpu_info = [&](bool supported)
	{
		info("GPU %d (%s) has compute %d.%d, %ssupport%s NVENC", idx, name.data(), major, minor,
			supported ? "" : "does not ",
			supported ? "s" : "");
		return supported;
	};

	if (make_version(major, minor) < make_version(3, 0)) // NVENC requires compute >= 3.0?
		return log_gpu_info(false);

	if (idx == -1 && minor == 0 && (major == 3 || major == 5)) { // recording from texture on compute 3.0 and 5.0 cards seems to be broken for some reason (tested on GTX 870M and GTX 750 Ti)
		info("Disabling texture input due to issues on compute %d.%d cards", major, minor);
		enc->use_texture_input = false;
	} else
		enc->use_texture_input = true;

	CUcontext ctx;
	if (res = enc->cuda->cuCtxCreate(&ctx, 0, device))
		return cuda_error("cuCtxCreate");

	enc->ctx.reset(ctx, [cuda = enc->cuda](CUcontext ctx) { cuda->cuCtxDestroy(ctx); });

	CUcontext current;
	if (res = enc->cuda->cuCtxPopCurrent(&current))
		return cuda_error("cuCtxPopCurrent");

	auto nvenc_supported = OpenSession(enc) && CheckCodec(enc) && CheckCapabilities(enc, allow_async);

	if (!log_gpu_info(nvenc_supported))
		return false;

	return true;
}

static bool OpenDevice(Encoder *enc, IDXGIAdapter *adapter, bool allow_async)
{
	CUDAResult res(enc->cuda);

	auto cuda_error = [&](const char *func)
	{
		warn("%s returned %s (%#x): %s", func, res.Name(), res.res, res.Description());
		return false;
	};

	CUdevice device;
	if (res = enc->cuda->cuD3D11GetDevice(&device, adapter))
		return cuda_error("cuD3D11GetDevice");

	return ActualOpenDevice(enc, device, allow_async, -1);
}

static bool OpenDevice(Encoder *enc, int idx, bool allow_async)
{
	CUDAResult res(enc->cuda);

	auto cuda_error = [&](const char *func)
	{
		warn("%s returned %s (%#x): %s", func, res.Name(), res.res, res.Description());
		return false;
	};

	CUdevice device;
	if (res = enc->cuda->cuDeviceGet(&device, idx))
		return cuda_error("cuDeviceGet");

	return ActualOpenDevice(enc, device, allow_async, idx);
}

static void GetVideoInfo(void *data, video_scale_info *info);

static bool InitEncoder(Encoder *enc)
{
	NV_ENC_PRESET_CONFIG preset_config = { 0 };
	preset_config.version = NV_ENC_PRESET_CONFIG_VER;
	preset_config.presetCfg.version = NV_ENC_CONFIG_VER;

	NVENCStatus sts;
	auto warn_status = [&](const char *func) { sts.Warn(enc, func); };

	if (sts = enc->funcs.nvEncGetEncodePresetConfig(enc->nv_encoder, enc->init_params.encodeGUID, enc->init_params.presetGUID, &preset_config)) {
		warn_status("nvEncGetEncodePresetConfig");
		return false;
	}

	enc->encode_config = preset_config.presetCfg;
	enc->encode_config.version = NV_ENC_CONFIG_VER;

	enc->encode_config.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;

	enc->encode_config.frameIntervalP = enc->b_frames_actual + 1;
	enc->encode_config.gopLength = static_cast<uint32_t>(enc->keyint_sec * enc->init_params.frameRateNum / enc->init_params.frameRateDen);

	{
		auto &rc = enc->encode_config.rcParams;
		rc.averageBitRate = enc->bitrate * 1000;
		rc.maxBitRate = enc->bitrate * 1000;
		rc.rateControlMode = enc->rc_mode;
	}

	{
		auto video = obs_encoder_video(enc->encoder);
		auto voi = video_output_get_info(video);

		video_scale_info vsi{};
		obs_encoder_get_active_video_conversion(enc->encoder, &vsi);
		GetVideoInfo(enc, &vsi);

		auto &h264 = enc->encode_config.encodeCodecConfig.h264Config;
		auto &vui = h264.h264VUIParameters;

		vui.colourMatrix = vsi.colorspace == VIDEO_CS_709 ? 1 : 5;
		vui.colourPrimaries = vsi.colorspace == VIDEO_CS_709 ? 1 : 5;
		vui.transferCharacteristics = vsi.colorspace == VIDEO_CS_709 ? 1 : 5;
		vui.colourDescriptionPresentFlag = 1;
		vui.videoSignalTypePresentFlag = 1;
		vui.chromaSampleLocationBot = 2;
		vui.chromaSampleLocationTop = 2;
		vui.chromaSampleLocationFlag = 1;

		h264.sliceMode = 3;
		h264.sliceModeData = 1;

		h264.disableSPSPPS = 1;
		h264.repeatSPSPPS = 0;

		if (enc->encode_config.rcParams.rateControlMode == NV_ENC_PARAMS_RC_CBR_LOWDELAY_HQ) {
			h264.outputBufferingPeriodSEI = 1;
			h264.outputPictureTimingSEI = 1;
		}

		h264.adaptiveTransformMode = NV_ENC_H264_ADAPTIVE_TRANSFORM_ENABLE;
		h264.fmoMode = NV_ENC_H264_FMO_DISABLE;

		h264.chromaFormatIDC = 1;
	}

	switch (enc->profile) {
	case H264Profile::Baseline:
		enc->encode_config.profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;
		break;
	case H264Profile::Main:
		enc->encode_config.profileGUID = NV_ENC_H264_PROFILE_MAIN_GUID;
		break;
	case H264Profile::High:
		enc->encode_config.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
		break;
	}

	if (sts = enc->funcs.nvEncInitializeEncoder(enc->nv_encoder, &enc->init_params)) {
		warn_status("nvEncInitializeEncoder");
		return false;
	}

	return true;
}

const static D3D_FEATURE_LEVEL feature_levels[] =
{
	D3D_FEATURE_LEVEL_11_0,
	D3D_FEATURE_LEVEL_10_1,
	D3D_FEATURE_LEVEL_10_0,
	D3D_FEATURE_LEVEL_9_3,
};

static bool InitD3DSurfaces(Encoder *enc, IDXGIAdapter *adapter)
{
	video_scale_info vsi{};
	if (!obs_encoder_get_active_video_conversion(enc->encoder, &vsi)) {
		error("Failed to get active video conversion");
		return false;
	}

	GetVideoInfo(enc, &vsi);
	
	video_texture_size vts{};
	if (!video_get_output_texture_size(&vsi, &vts)) {
		error("Failed to get output_texture_size");
		return false;
	}

	enc->input_format = vsi.format;

	auto width = vts.width;
	auto height = vts.height;

	CUDAResult res(enc->cuda);

	auto cuda_error = [&](const char *func)
	{
		warn("%s returned %s (%#x): %s", func, res.Name(), res.res, res.Description());
		return false;
	};

	auto hr_error = [&](HRESULT res, const char *name)
	{
		if (res == S_OK)
			return false;

		warn("%s failed: %#x", name, res);
		return true;
	};

#define HR_ERROR(x) hr_error(x, #x)
	D3D_FEATURE_LEVEL level_used;
	ComPtr<ID3D11DeviceContext> context;
	if (HR_ERROR(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
		feature_levels, sizeof(feature_levels) / sizeof(feature_levels[0]), D3D11_SDK_VERSION, enc->d3d11_device.Assign(), &level_used, context.Assign())))
		return false;

	return true;
}

static bool InitSurfaces(Encoder *enc, IDXGIAdapter *adapter)
{
	CUDAResult res(enc->cuda);
	bool pushed_ctx = false;
	if (enc->use_texture_input) {
		if (res = enc->cuda->cuCtxPushCurrent(enc->ctx.get())) {
			error("Encode: cuCtxPushCurrent returned %s (%d): %s", res.Name(), res.res, res.Description());
			return false;
		}
		pushed_ctx = true;
	}

	auto pop_context_impl = [&]
	{
		if (!pushed_ctx)
			return true;

		CUcontext dummy;
		if (res = enc->cuda->cuCtxPopCurrent(&dummy)) {
			error("Encode: cuCtxPopCurrent returned %s (%d): %s", res.Name(), res.res, res.Description());
			return false;
		}

		return true;
	};

	auto context_guard = guard(pop_context_impl);

	auto cuda_error = [&](const char *func)
	{
		warn("%s returned %s (%#x): %s", func, res.Name(), res.res, res.Description());
		return false;
	};

	auto width = obs_encoder_get_width(enc->encoder);
	auto height = obs_encoder_get_height(enc->encoder);

	if (enc->use_texture_input && !InitD3DSurfaces(enc, adapter))
		return false;

	auto surfaces = max(4, enc->encode_config.frameIntervalP * 2 * 2); // 2 NVENC encode sessions per GPU * 2 to decrease likelihood of blocking next group of frames

	enc->surfaces.resize(surfaces);

	NV_ENC_EVENT_PARAMS evt_params{};
	evt_params.version = NV_ENC_EVENT_PARAMS_VER;


	for (auto &surface : enc->surfaces) {
		{
			if (enc->init_params.enableEncodeAsync) {
				surface.event.reset(CreateEvent(nullptr, false, false, nullptr));

				evt_params.completionEvent = surface.event.get();
				if (NVENCStatus sts = enc->funcs.nvEncRegisterAsyncEvent(enc->nv_encoder, &evt_params)) {
					sts.Warn(enc, "nvEncRegisterAsyncEvent");
					return false;
				}
			}

			if (enc->use_texture_input) {
				switch (enc->input_format) {
				case VIDEO_FORMAT_RGBA:
					surface.copy_width = width * 4;
					surface.copy_height = height;
					surface.format = NV_ENC_BUFFER_FORMAT_ABGR;
					break;
				case VIDEO_FORMAT_NV12:
					surface.copy_width = width;
					surface.copy_height = height * 3 / 2;
					surface.format = NV_ENC_BUFFER_FORMAT_NV12;
					break;
				}
				if (res = enc->cuda->cuMemAllocPitch(&surface.cuda_memory, &surface.cuda_pitch, surface.copy_width, surface.copy_height, 16))
					return cuda_error("cuMemAllocPitch");

				NV_ENC_REGISTER_RESOURCE reg{};
				reg.version = NV_ENC_REGISTER_RESOURCE_VER;
				reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
				reg.resourceToRegister = reinterpret_cast<void*>(surface.cuda_memory);
				reg.width = width;
				reg.height = height;
				reg.pitch = surface.cuda_pitch;
				reg.bufferFormat = surface.format;
				if (NVENCStatus sts = enc->funcs.nvEncRegisterResource(enc->nv_encoder, &reg)) {
					sts.Warn(enc, "nvEncRegisterResource");
					return false;
				}

				surface.registered_cuda_memory = reg.registeredResource;

				surface.width = width;
				surface.height = height;

			} else {
				NV_ENC_CREATE_INPUT_BUFFER alloc_in = { 0 };
				alloc_in.version = NV_ENC_CREATE_INPUT_BUFFER_VER;

				auto aligned_width = (width + 31) & ~31;
				auto aligned_height = (height + 31) & ~31;

				alloc_in.width = aligned_width;
				alloc_in.height = aligned_height;
				alloc_in.bufferFmt = surface.format;

				if (NVENCStatus sts = enc->funcs.nvEncCreateInputBuffer(enc->nv_encoder, &alloc_in)) {
					sts.Warn(enc, "nvEncCreateInputBuffer");
					return false;
				}

				surface.input = alloc_in.inputBuffer;
				surface.width = alloc_in.width;
				surface.height = alloc_in.height;
			}
		}

		{
			NV_ENC_CREATE_BITSTREAM_BUFFER alloc_out = { 0 };
			alloc_out.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;

			if (NVENCStatus sts = enc->funcs.nvEncCreateBitstreamBuffer(enc->nv_encoder, &alloc_out)) {
				sts.Warn(enc, "nvEncCreateBitstreamBuffer");
				return false;
			}

			surface.output = alloc_out.bitstreamBuffer;
		}

		enc->idle.push_back(&surface);
	}

	return true;
}

static bool InitSPSPPS(Encoder *enc)
{
	NV_ENC_SEQUENCE_PARAM_PAYLOAD payload = { 0 };
	payload.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;

	array<uint8_t, 512> buffer;
	uint32_t size = 0;

	payload.spsppsBuffer = buffer.data();
	payload.inBufferSize = buffer.size();
	payload.outSPSPPSPayloadSize = &size;

	if (NVENCStatus sts = enc->funcs.nvEncGetSequenceParams(enc->nv_encoder, &payload)) {
		sts.Warn(enc, "nvEncGetSequenceParams");
		return false;
	}

	enc->headers.assign(buffer.data(), buffer.data() + size);
	return true;
}

static bool EncoderUpdate(void *context, obs_data_t *settings)
{
	auto enc = cast(context);
	if (!enc->dynamic_bitrate)
		return false;

	auto old_bitrate = enc->bitrate;
	auto restore_config = guard([&, encode_config = enc->encode_config]
	{
		enc->bitrate = old_bitrate;
		enc->encode_config = encode_config;
	});

	enc->bitrate = static_cast<uint32_t>(obs_data_get_int(settings, "bitrate"));

	NV_ENC_RECONFIGURE_PARAMS params = { 0 };
	params.version = NV_ENC_RECONFIGURE_PARAMS_VER;
	params.reInitEncodeParams = enc->init_params;

	{
		auto &rc = enc->encode_config.rcParams;
		rc.averageBitRate = enc->bitrate * 1000;
		rc.maxBitRate = enc->bitrate * 1000;
	}

	if (NVENCStatus sts = enc->funcs.nvEncReconfigureEncoder(enc->nv_encoder, &params)) {
		sts.Warn(enc, "nvEncReconfigureEncoder");
		warn("EncoderUpdate: failed to update bitrate from %d to %d", old_bitrate, enc->bitrate);
		return false;
	}

	restore_config.dismiss();
	info("EncoderUpdate: changed bitrate from %d to %d", old_bitrate, enc->bitrate);
	return true;
}

static void EncoderDestroy(void *context)
try {
	delete cast(context);
} catch (...) {
	blog(LOG_ERROR, "[NVENC/Encoder]: EncoderDestroy: unhandled exception");
}

static void *EncoderCreate(obs_data_t *settings, obs_encoder_t *encoder)
try {
	auto enc = make_unique<Encoder>(encoder);

	if (!LoadFunctions(enc.get()))
		return nullptr;

	ComPtr<IDXGIAdapter> adapter;
	[&]
	{
		obs_enter_graphics();
		DEFER{ obs_leave_graphics(); };

		auto hr_error = [&](HRESULT res, const char *name)
		{
			if (res == S_OK)
				return false;

			warn("%s failed: %#x", name, res);
			return true;
		};
		
		auto device_type = gs_get_device_type();
		if (device_type != GS_DEVICE_DIRECT3D_11) {
			info("Not using OBS D3D11 backend: %d", device_type);
			return;
		}

#define HR_ERROR(x) hr_error(x, #x)
		ComPtr<IDXGIDevice> dxgi_device;
		auto device = reinterpret_cast<ID3D11Device*>(gs_get_device_handle());
		if (HR_ERROR(device->QueryInterface(dxgi_device.Assign())))
			return;

		HR_ERROR(dxgi_device->GetAdapter(adapter.Assign()));
#undef HR_ERROR
	}();

	CUDAResult res(enc->cuda);

	if (res = enc->cuda->cuInit(0)) {
		warn("cuInit returned %s (%d): %s", res.Name(), res.res, res.Description());
		return nullptr;
	}

	int device_count = 0;
	if (res = enc->cuda->cuDeviceGetCount(&device_count)) {
		warn("cuDeviceGetCount returned %s (%d): %s", res.Name(), res.res, res.Description());
		return nullptr;
	}

	if (!device_count) {
		warn("No CUDA capable devices found");
		return nullptr;
	}

	LoadConfig(enc.get(), settings);

	bool allow_async = true;

	auto maybe_retry_without_async = [&]
	{
		if (!enc->init_params.enableEncodeAsync)
			return false;
		enc->init_params.enableEncodeAsync = false;
		allow_async = false;
		warn("Initialization failed with ASYNC_ENCODE enabled, retrying");
		enc->Release();
		return true;
	};

	int device = -1;
	do {
		bool have_device = false;
		if (adapter && OpenDevice(enc.get(), adapter, allow_async)) {
			have_device = true;
		} else {
			enc->allow_texture_input = false;
			device = 0;
			for (; device < device_count; device++)
				if (OpenDevice(enc.get(), device, allow_async)) {
					have_device = true;
					break;
				}
		}

		if (!have_device) {
			warn("No valid device found");
			return nullptr;
		}

		if (!InitEncoder(enc.get())) {
			if (maybe_retry_without_async())
				continue;

			return nullptr;
		}

		if (!InitSurfaces(enc.get(), adapter)) {
			if (maybe_retry_without_async())
				continue;

			return nullptr;
		}

		break;
	} while (true);

	if (!InitSPSPPS(enc.get()))
		return nullptr;


	{
		auto preset_name = "default"s;
		for (auto &preset : presets)
			if (preset.guid == enc->init_params.presetGUID)
				preset_name = preset.name;
		boost::to_lower(preset_name);

		auto profile = enc->profile == H264Profile::Baseline ? "baseline" :
			enc->profile == H264Profile::Main ? "main" : "high";

		info("Encoder settings:\n"
			"\tbitrate:      %d\n"
			"\tkeyint:       %d (%g s)\n"
			"\tpreset:       %s\n"
			"\tprofile:      %s\n"
			"\twidth:        %d\n"
			"\theight:       %d\n"
			"\trate-control: %s\n"
			"\tb-frames:     %d%s\n"
			"\tGPU:          %d\n"
			"\tTextures:     %s",
			enc->bitrate, enc->encode_config.gopLength, enc->keyint_sec, preset_name.c_str(), profile,
			obs_encoder_get_width(encoder), obs_encoder_get_height(encoder),
			enc->rc_mode == NV_ENC_PARAMS_RC_CBR_LOWDELAY_HQ ? "CBR" : "VBR",
			enc->b_frames_actual, enc->b_frames_actual != enc->b_frames ? (" (requested: " + to_string(enc->b_frames) + ")").c_str() : "",
			device, enc->use_texture_input ? "yes" : "no");
	}

	return enc.release();

} catch (const pair<const char*, int> &err) {
	do_log(LOG_WARNING, encoder, "Failed to create encoder: %s (line %d)", err.first, err.second);
	return nullptr;
} catch (...) {
	do_log(LOG_WARNING, encoder, "Failed to create encoder");
	return nullptr;
}

static bool EncoderHeaders(void *data, uint8_t **extra_data, size_t *size)
{
	auto enc = cast(data);
	*extra_data = enc->headers.data();
	*size = enc->headers.size();

	return !enc->headers.empty();
}

static bool CopyFrame(Encoder *enc, encoder_frame *frame, Surface *surface)
{
	ProfileScope("CopyFrame");

	if (surface->input) {
		if (NVENCStatus sts = enc->funcs.nvEncUnmapInputResource(enc->nv_encoder, surface->input))
			sts.Warn(enc, "nvEncUnmapInputResource");
		surface->input = nullptr;
	}

	{
		CUDAResult res(enc->cuda);

		auto cuda_error = [&](const char *func)
		{
			warn("%s returned %s (%#x): %s", func, res.Name(), res.res, res.Description());
			return false;
		};

		auto hr_error = [&](HRESULT res, const char *name)
		{
			if (res == S_OK)
				return false;

			warn("%s failed: %#x", name, res);
			return true;
		};

		auto &shared_tex = enc->input_textures[frame->tex];
		if (shared_tex.shared_handle && shared_tex.shared_handle != frame->shared_handle) {
			enc->cuda->cuGraphicsUnregisterResource(shared_tex.cuda_tex);
			shared_tex.d3d11_tex.Release();
			shared_tex.shared_handle = 0;
		}

#define HR_ERROR(x) hr_error(x, #x)
		if (!shared_tex.d3d11_tex) {
			ProfileScope("ID3D11Device::OpenSharedResource");
			if (HR_ERROR(enc->d3d11_device->OpenSharedResource(reinterpret_cast<HANDLE>(frame->shared_handle), __uuidof(ID3D11Texture2D), (void**)shared_tex.d3d11_tex.Assign())))
				return false;

			shared_tex.shared_handle = frame->shared_handle;

			if (res = enc->cuda->cuGraphicsD3D11RegisterResource(&shared_tex.cuda_tex, shared_tex.d3d11_tex, CU_GRAPHICS_REGISTER_FLAGS_TEXTURE_GATHER)) {
				cuda_error("cuGraphicsD3D11RegisterResource");
				return false;
			}
		}
#undef HR_ERROR

		ProfileScope("cuMemcpy2D");

		if (res = enc->cuda->cuGraphicsMapResources(1, &shared_tex.cuda_tex, 0))
			return cuda_error("cuGraphicsMapResources");

		auto finalize_interop_impl = [&]
		{
			if (res = enc->cuda->cuGraphicsUnmapResources(1, &shared_tex.cuda_tex, 0))
				cuda_error("cuGraphicsUnmapResources");

			if (res = enc->cuda->cuStreamSynchronize(0))
				cuda_error("cuStreamSynchronize");
		};

		DEFER{ finalize_interop_impl(); };

		CUarray array;
		if (res = enc->cuda->cuGraphicsSubResourceGetMappedArray(&array, shared_tex.cuda_tex, 0, 0))
			return cuda_error("cuGraphicsSubResourceGetMappedArray");

		auto width = enc->init_params.encodeWidth;
		auto height = enc->init_params.encodeHeight;

		CUDA_MEMCPY2D memcpy2d{};
		memcpy2d.dstDevice = surface->cuda_memory;
		memcpy2d.dstMemoryType = CU_MEMORYTYPE_DEVICE;
		memcpy2d.dstPitch = surface->cuda_pitch;

		memcpy2d.srcArray = array;
		memcpy2d.srcMemoryType = CU_MEMORYTYPE_ARRAY;
		memcpy2d.srcPitch = surface->copy_width;

		memcpy2d.Height = surface->copy_height;
		memcpy2d.WidthInBytes = surface->copy_width;

		if (res = enc->cuda->cuMemcpy2D(&memcpy2d))
			return cuda_error("cuMemcpy2D");
	}

	ProfileScope("nvEncMapInputResource");

	NV_ENC_MAP_INPUT_RESOURCE map{};
	map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
	map.registeredResource = surface->registered_cuda_memory;
	if (NVENCStatus sts = enc->funcs.nvEncMapInputResource(enc->nv_encoder, &map)) {
		sts.Warn(enc, "nvEncMapInputResource");
		return false;
	}
	surface->input = map.mappedResource;

	return true;
}

static bool UploadFrame(Encoder *enc, encoder_frame *frame, Surface *surface)
{
	NV_ENC_LOCK_INPUT_BUFFER lock = { 0 };
	lock.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
	lock.inputBuffer = surface->input;

	NVENCStatus sts;
	if (sts = enc->funcs.nvEncLockInputBuffer(enc->nv_encoder, &lock)) {
		sts.Warn(enc, "nvEncLockInputBuffer");
		return false;
	}

	surface->pitch = lock.pitch;
	for (auto plane = 0; plane < 2; plane++) { // NV12 only has 2 planes
		if (!frame->data[plane])
			continue;

		auto lines = enc->init_params.encodeHeight;
		if (plane > 0)
			lines >>= 1;

		auto base_target = reinterpret_cast<uint8_t*>(lock.bufferDataPtr) + plane * lock.pitch * surface->height;
		auto copy_size = min(lock.pitch, frame->linesize[plane]);
		for (uint32_t line = 0; line < lines; line++)
			memcpy(base_target + lock.pitch * line, frame->data[plane] + frame->linesize[plane] * line, copy_size);
	}

	if (sts = enc->funcs.nvEncUnlockInputBuffer(enc->nv_encoder, lock.inputBuffer)) {
		sts.Warn(enc, "nvEncUnlockInputBuffer");
		return false;
	}

	return true;
}

static bool ProcessOutput(Encoder *enc, encoder_packet *packet)
{
	if (!enc->b_frame_pts_calculated && enc->b_frames_actual && enc->timestamps.size() < 2)
		return false;

	if ((enc->init_params.enableEncodeAsync && enc->processing.empty()) || (!enc->init_params.enableEncodeAsync && enc->ready.empty()))
		return false;

	Surface *output;

	if (enc->init_params.enableEncodeAsync) {
		output = enc->processing.front();
		if (WaitForSingleObject(output->event.get(), enc->idle.empty() ? INFINITE : 0) != WAIT_OBJECT_0)
			return false;
		enc->processing.pop_front();
		enc->idle.push_back(output);
	} else {
		output = enc->ready.front();
		enc->ready.pop_front();
		enc->idle.push_back(output);
	}

	// always handle dts, even if this particular bitstream errors in some way?
	DEFER{
		if (enc->b_frames_actual && !enc->b_frame_pts_calculated) {
			auto ts0 = static_cast<int64_t>(enc->timestamps[0]);
			auto ts1 = static_cast<int64_t>(enc->timestamps[1]);
			auto delta = ts1 - ts0;

			enc->b_frame_pts_calculated = true;

			packet->dts = ts0 - delta;
		} else {
			packet->dts = enc->timestamps.front();
			enc->timestamps.pop_front();
		}
	};

	NV_ENC_LOCK_BITSTREAM lock = { 0 };
	lock.version = NV_ENC_LOCK_BITSTREAM_VER;

	{
		enc->slice_offsets.resize(enc->encode_config.encodeCodecConfig.h264Config.sliceModeData);

		lock.outputBitstream = output->output;
		lock.sliceOffsets = enc->slice_offsets.data();

		if (NVENCStatus sts = enc->funcs.nvEncLockBitstream(enc->nv_encoder, &lock)) {
			sts.Warn(enc, "nvEncLockBitstream");
			return false;
		}

		auto unlock_bitstream_impl = [&]
		{
			if (NVENCStatus sts = enc->funcs.nvEncUnlockBitstream(enc->nv_encoder, output->output)) {
				sts.Warn(enc, "nvEncUnlockBitstream");
				return false;
			}

			return true;
		};

		DEFER{ unlock_bitstream_impl(); };

		enc->slice_data.resize(lock.bitstreamSizeInBytes);

		auto bitstream_ptr = reinterpret_cast<uint8_t*>(lock.bitstreamBufferPtr);
		enc->slice_data.assign(bitstream_ptr, bitstream_ptr + lock.bitstreamSizeInBytes);
	}

	if (enc->use_texture_input && output->input) {
		if (NVENCStatus sts = enc->funcs.nvEncUnmapInputResource(enc->nv_encoder, output->input))
			sts.Warn(enc, "nvEncUnmapInputResource");
		output->input = nullptr;
	}

	packet->data = enc->slice_data.data();
	packet->size = enc->slice_data.size();
	packet->type = OBS_ENCODER_VIDEO;
	packet->keyframe = obs_avc_keyframe(packet->data, packet->size);

	packet->pts = lock.outputTimeStamp;

	return true;
}

static bool Encode(void *context, encoder_frame *frame, encoder_packet *packet, bool *received_packet)
try {
	auto enc = cast(context);

	bool have_output = false;
	if (enc->init_params.enableEncodeAsync && enc->idle.empty())
		have_output = *received_packet = ProcessOutput(enc, packet);

	bool encode_success = false; // as opposed to NEED_MORE_INPUT

	if (frame) {
		if (enc->idle.empty()) {
			error("Encode: no idle surfaces while trying to encode frame");
			return false;
		}

		auto input = enc->idle.front();

		CUDAResult res(enc->cuda);
		if (res = enc->cuda->cuCtxPushCurrent(enc->ctx.get())) {
			error("Encode: cuCtxPushCurrent returned %s (%d): %s", res.Name(), res.res, res.Description());
			return false;
		}

		auto pop_context_impl = [&]
		{
			CUcontext dummy;
			if (res = enc->cuda->cuCtxPopCurrent(&dummy)) {
				error("Encode: cuCtxPopCurrent returned %s (%d): %s", res.Name(), res.res, res.Description());
				return false;
			}

			return true;
		};

		auto context_guard = guard(pop_context_impl);

		auto pop_context = [&]
		{
			context_guard.dismiss();
			return pop_context_impl();
		};

		if (frame->is_texture != enc->use_texture_input) {
			error("frame->is_texture (%d) does not match use_texture_input (%d)", frame->is_texture, enc->use_texture_input);
			return false;
		}

		if (enc->use_texture_input) {
			if (!CopyFrame(enc, frame, input)) {
				error("CopyFrame failed");
				return false;
			}
		} else {
			if (!UploadFrame(enc, frame, input)) {
				error("UploadFrame failed");
				return false;
			}
		}
		
		NV_ENC_PIC_PARAMS pic = { 0 };
		pic.version = NV_ENC_PIC_PARAMS_VER;
		pic.inputBuffer = input->input;
		pic.inputWidth = input->width;
		pic.inputHeight = input->height;
		pic.inputPitch = input->pitch;
		pic.outputBitstream = input->output;
		pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
		pic.inputTimeStamp = frame->pts;

		pic.codecPicParams.h264PicParams.sliceMode = enc->encode_config.encodeCodecConfig.h264Config.sliceMode;
		pic.codecPicParams.h264PicParams.sliceModeData = enc->encode_config.encodeCodecConfig.h264Config.sliceModeData;

		if (enc->init_params.enableEncodeAsync)
			pic.completionEvent = input->event.get();

		if (NVENCStatus sts = enc->funcs.nvEncEncodePicture(enc->nv_encoder, &pic)) {
			if (sts.sts != NV_ENC_ERR_NEED_MORE_INPUT) {
				sts.Error(enc, "nvEncEncodePicture");
				return false;
			}
		} else {
			encode_success = true;
		}

		enc->timestamps.push_back(frame->pts);

		enc->processing.push_back(input);
		enc->idle.pop_front();

		if (!pop_context())
			return false;
	}

	if (encode_success && !enc->init_params.enableEncodeAsync) {
		enc->ready.insert(end(enc->ready), begin(enc->processing), end(enc->processing));
		enc->processing.clear();
	}

	if (!have_output)
		*received_packet = ProcessOutput(enc, packet);

	return true;

} catch (...) {
	auto enc = cast(context);
	warn("Encode: unhandled exception");
	return false;
}

static void GetVideoInfo(void *data, video_scale_info *info)
{
	auto enc = cast(data);
	if (!enc || !enc->use_texture_input) {
		info->format = VIDEO_FORMAT_NV12;
		return;
	}

	switch (info->format) {
	case VIDEO_FORMAT_NV12:
		break;
	case VIDEO_FORMAT_RGBA:
		if (enc->allow_rgba_texture_input)
			break;
	default:
		info->format = VIDEO_FORMAT_NV12;
		break;
	}

	info->texture_output = true;
}

void RegisterNVENCEncoder()
{
	CUDAFunctions cuda;
	NVENCFunctions nvenc;
	if (!cuda.Load() || !nvenc.Load()) {
		blog(LOG_INFO, "Crucible NVENC not available");
		return;
	}

	obs_encoder_info info = { 0 };
	info.id = "crucible_nvenc";
	info.type = OBS_ENCODER_VIDEO;
	info.codec = "h264";
	info.get_name = [](void*) { return "Crucible NVENC"; };
	info.create = EncoderCreate;
	info.update = EncoderUpdate;
	info.get_defaults = EncoderDefaults;
	info.get_extra_data = EncoderHeaders;
	info.destroy = EncoderDestroy;
	info.encode = Encode;
	info.get_video_info = GetVideoInfo;

	obs_register_encoder(&info);

	blog(LOG_INFO, "Crucible NVENC registered");
}
