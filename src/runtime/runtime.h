#pragma once
#ifdef _WIN32
#ifdef POSEWEAVER_RUNTIME_BUILD
#define PW_API __declspec(dllexport)
#else
#define PW_API __declspec(dllimport)
#endif
#else
#define PW_API __attribute__((visibility("default")))
#endif
extern "C" {
struct PwRuntime;
PW_API PwRuntime* pw_create();
PW_API void pw_destroy(PwRuntime* runtime);
// UTF-8 reply belongs to this instance, valid until its next request. One calling thread per instance.
PW_API const char* pw_request(PwRuntime* runtime,const char* request);
}
