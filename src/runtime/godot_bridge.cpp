#include "runtime/runtime.h"
#include "gdextension_interface.h"
#include <string>
namespace {
GDExtensionClassLibraryPtr library;
GDExtensionInterfaceStringNameNewWithUtf8Chars nameNew;
GDExtensionInterfaceStringNewWithUtf8Chars stringNew;
GDExtensionInterfaceStringToUtf8Chars stringUtf8;
GDExtensionInterfaceClassdbConstructObject construct;
GDExtensionInterfaceObjectSetInstance setInstance;
GDExtensionInterfaceClassdbRegisterExtensionClass2 registerClass;
GDExtensionInterfaceClassdbRegisterExtensionClassMethod registerMethod;
GDExtensionInterfaceClassdbUnregisterExtensionClass unregisterClass;
GDExtensionInterfaceVariantGetType variantType;
GDExtensionTypeFromVariantConstructorFunc toString;
GDExtensionVariantFromTypeConstructorFunc fromString;
GDExtensionPtrDestructor nameDestroy,stringDestroy;
struct Name { alignas(8) unsigned char bytes[16]; Name(const char* value) { nameNew(bytes,value); } ~Name() { nameDestroy(bytes); } };
GDExtensionObjectPtr create(void*) {
    Name parent("RefCounted"),type("PoseweaverNative"); auto object=construct(parent.bytes);
    auto* runtime=pw_create(); if (!runtime) return nullptr;
    setInstance(object,type.bytes,runtime); return object;
}
void freeInstance(void*,GDExtensionClassInstancePtr instance) { pw_destroy(static_cast<PwRuntime*>(instance)); }
GDExtensionClassCallVirtual getVirtual(void*,GDExtensionConstStringNamePtr) { return nullptr; }
const char* request(GDExtensionClassInstancePtr instance,GDExtensionConstStringPtr value) {
    const char* reply="{\"ok\":false,\"error\":\"Invalid request\"}";
    try {
        auto length=stringUtf8(value,nullptr,0);
        if (length>=0 && length<=32*1024*1024) {
            std::string text((size_t)length,'\0'); stringUtf8(value,text.data(),length);
            reply=pw_request(static_cast<PwRuntime*>(instance),text.c_str());
        }
    } catch (...) { reply="{\"ok\":false,\"error\":\"Request conversion failed\"}"; }
    return reply;
}
void exchangePointer(void*,GDExtensionClassInstancePtr instance,const GDExtensionConstTypePtr* args,GDExtensionTypePtr result) {
    stringNew(result,request(instance,args[0]));
}
void exchange(void*,GDExtensionClassInstancePtr instance,const GDExtensionConstVariantPtr* args,GDExtensionInt count,GDExtensionVariantPtr result,GDExtensionCallError* error) {
    const char* reply="{\"ok\":false,\"error\":\"Expected one String request\"}";
    if (count==1 && variantType(args[0])==GDEXTENSION_VARIANT_TYPE_STRING) {
        alignas(8) unsigned char string[16]; toString(string,const_cast<void*>(args[0]));
        reply=request(instance,string);
        stringDestroy(string);
    }
    alignas(8) unsigned char value[16]; stringNew(value,reply); fromString(result,value); stringDestroy(value);
    if (error) error->error=GDEXTENSION_CALL_OK;
}
void initialize(void*,GDExtensionInitializationLevel level) {
    if (level!=GDEXTENSION_INITIALIZATION_SCENE) return;
    Name type("PoseweaverNative"),parent("RefCounted"),method("exchange"),argName("request"),empty("");
    GDExtensionClassCreationInfo2 info{}; info.is_exposed=true; info.create_instance_func=create; info.free_instance_func=freeInstance; info.get_virtual_func=getVirtual;
    registerClass(library,type.bytes,parent.bytes,&info);
    GDExtensionPropertyInfo property{}; property.type=GDEXTENSION_VARIANT_TYPE_STRING; property.name=argName.bytes; property.class_name=empty.bytes;
    alignas(8) unsigned char hint[16]; stringNew(hint,""); property.hint_string=hint;
    GDExtensionClassMethodArgumentMetadata metadata=GDEXTENSION_METHOD_ARGUMENT_METADATA_NONE;
    GDExtensionClassMethodInfo call{}; call.name=method.bytes; call.call_func=exchange; call.ptrcall_func=exchangePointer; call.method_flags=GDEXTENSION_METHOD_FLAG_NORMAL;
    call.has_return_value=true; call.return_value_info=&property; call.argument_count=1; call.arguments_info=&property; call.arguments_metadata=&metadata;
    registerMethod(library,type.bytes,&call); stringDestroy(hint);
}
void deinitialize(void*,GDExtensionInitializationLevel level) { if (level==GDEXTENSION_INITIALIZATION_SCENE) { Name type("PoseweaverNative"); unregisterClass(library,type.bytes); } }
}
extern "C" PW_API GDExtensionBool poseweaver_library_init(GDExtensionInterfaceGetProcAddress get,GDExtensionClassLibraryPtr lib,GDExtensionInitialization* init) {
    library=lib;
#define GET(field,type,name) field=reinterpret_cast<type>(get(name)); if (!field) return false
    GET(nameNew,GDExtensionInterfaceStringNameNewWithUtf8Chars,"string_name_new_with_utf8_chars");
    GET(stringNew,GDExtensionInterfaceStringNewWithUtf8Chars,"string_new_with_utf8_chars");
    GET(stringUtf8,GDExtensionInterfaceStringToUtf8Chars,"string_to_utf8_chars");
    GET(construct,GDExtensionInterfaceClassdbConstructObject,"classdb_construct_object");
    GET(setInstance,GDExtensionInterfaceObjectSetInstance,"object_set_instance");
    GET(registerClass,GDExtensionInterfaceClassdbRegisterExtensionClass2,"classdb_register_extension_class2");
    GET(registerMethod,GDExtensionInterfaceClassdbRegisterExtensionClassMethod,"classdb_register_extension_class_method");
    GET(unregisterClass,GDExtensionInterfaceClassdbUnregisterExtensionClass,"classdb_unregister_extension_class");
    GET(variantType,GDExtensionInterfaceVariantGetType,"variant_get_type");
    auto getDestructor=reinterpret_cast<GDExtensionInterfaceVariantGetPtrDestructor>(get("variant_get_ptr_destructor"));
    auto getTo=reinterpret_cast<GDExtensionInterfaceGetVariantToTypeConstructor>(get("get_variant_to_type_constructor"));
    auto getFrom=reinterpret_cast<GDExtensionInterfaceGetVariantFromTypeConstructor>(get("get_variant_from_type_constructor"));
    if (!getDestructor || !getTo || !getFrom) return false;
    nameDestroy=getDestructor(GDEXTENSION_VARIANT_TYPE_STRING_NAME); stringDestroy=getDestructor(GDEXTENSION_VARIANT_TYPE_STRING);
    toString=getTo(GDEXTENSION_VARIANT_TYPE_STRING); fromString=getFrom(GDEXTENSION_VARIANT_TYPE_STRING);
    init->initialize=initialize; init->deinitialize=deinitialize; init->userdata=nullptr; init->minimum_initialization_level=GDEXTENSION_INITIALIZATION_SCENE;
    return nameDestroy && stringDestroy && toString && fromString;
}
