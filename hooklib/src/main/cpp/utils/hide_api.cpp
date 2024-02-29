//
// Created by swift on 2019/1/21.
//
#include "../includes/hide_api.h"
#include "../includes/arch.h"
#include "../includes/elf_util.h"
#include "../includes/log.h"
#include "../includes/utils.h"
#include "../includes/trampoline_manager.h"
#include "../includes/art_runtime.h"
#include "shadowhook.h"

extern int SDK_INT;

extern "C" {


    void* jitCompilerHandle = nullptr;
    bool (*jitCompileMethod)(void*, void*, void*, bool) = nullptr;
    bool (*jitCompileMethodQ)(void* jit, void* artMethod, void* self, bool baseline, bool osr) = nullptr;
    bool (*jitCompileMethodR)(void* jit, void* artMethod, void* self, int compilationKind, bool prejit) = nullptr;

    void (*innerSuspendVM)() = nullptr;
    void (*innerResumeVM)() = nullptr;

    jobject (*addWeakGlobalRef)(JavaVM *, void *, void *) = nullptr;

    art::jit::JitCompiler** globalJitCompileHandlerAddr = nullptr;

    //for Android Q
    void (**origin_jit_update_options)(void *) = nullptr;

    void (*profileSaver_ForceProcessProfiles)() = nullptr;

    //for Android R
    void *jniIdManager = nullptr;
    ArtMethod *(*origin_DecodeArtMethodId)(void *thiz, jmethodID jmethodId) = nullptr;
    ArtMethod *replace_DecodeArtMethodId(void *thiz, jmethodID jmethodId) {
        SHADOWHOOK_STACK_SCOPE();
        jniIdManager = thiz;
        SHADOWHOOK_ALLOW_REENTRANT();
        ArtMethod * ret = origin_DecodeArtMethodId(thiz, jmethodId);
        SHADOWHOOK_DISALLOW_REENTRANT();
        return ret;
    }

    // TODO: ShouldUseInterpreterEntrypoint 无法帮助函数内联时的 hook，已弃用
    bool (*origin_ShouldUseInterpreterEntrypoint)(ArtMethod *artMethod, const void* quick_code) = nullptr;
    bool replace_ShouldUseInterpreterEntrypoint(ArtMethod *artMethod, const void* quick_code) {
        SHADOWHOOK_STACK_SCOPE();
        if (SandHook::TrampolineManager::get().methodHooked(artMethod) && quick_code != nullptr) {
            return false;
        }
        SHADOWHOOK_ALLOW_REENTRANT();
        bool ret = origin_ShouldUseInterpreterEntrypoint(artMethod, quick_code);
        SHADOWHOOK_DISALLOW_REENTRANT();
        return ret;
    }

    // paths
    const char* art_lib_path;
    const char* jit_lib_path;

    JavaVM* jvm;

    void (*class_init_callback)(void*) = nullptr;

    void (*backup_fixup_static_trampolines)(void *, void *) = nullptr;

    void *(*backup_mark_class_initialized)(void *, void *, uint32_t *) = nullptr;

    void (*backup_update_methods_code)(void *, ArtMethod *, const void *) = nullptr;

    void* (*make_initialized_classes_visibly_initialized_)(void*, void*, bool) = nullptr;

    void* runtime_instance_ = nullptr;

    void* class_linker_ = nullptr;

    void* (*origin_CreateProxyClass)(void* thiz,
                                     void* soa,
                                     jstring name,
                                     jobjectArray interfaces,
                                     jobject loader,
                                     jobjectArray methods,
                                     jobjectArray throws);
    void* replace_CreateProxyClass(void* thiz,
                                   void* soa,
                                   jstring name,
                                   jobjectArray interfaces,
                                   jobject loader,
                                   jobjectArray methods,
                                   jobjectArray throws) {
        SHADOWHOOK_STACK_SCOPE();
        SHADOWHOOK_ALLOW_REENTRANT();
        void* ret = nullptr;
        if (origin_CreateProxyClass != nullptr) {
            class_linker_ = thiz;
            ret = origin_CreateProxyClass(thiz, soa, name, interfaces, loader, methods, throws);
        }
        SHADOWHOOK_DISALLOW_REENTRANT();
        return ret;
    }

    void initHideApi(JNIEnv* env) {

        env->GetJavaVM(&jvm);
        shadowhook_init(SHADOWHOOK_MODE_SHARED, false);

        if (BYTE_POINT == 8) {
            if (SDK_INT >= ANDROID_Q) {
                art_lib_path = "/lib64/libart.so";
                jit_lib_path = "/lib64/libart-compiler.so";
            } else {
                art_lib_path = "/system/lib64/libart.so";
                jit_lib_path = "/system/lib64/libart-compiler.so";
            }
        } else {
            if (SDK_INT >= ANDROID_Q) {
                art_lib_path = "/lib/libart.so";
                jit_lib_path = "/lib/libart-compiler.so";
             } else {
                art_lib_path = "/system/lib/libart.so";
                jit_lib_path = "/system/lib/libart-compiler.so";
            }
        }

        //init compile
        if (SDK_INT >= ANDROID_N) {
            globalJitCompileHandlerAddr = reinterpret_cast<art::jit::JitCompiler **>(
                    getSymCompat(art_lib_path,
                                 "_ZN3art3jit3Jit20jit_compiler_handle_E"));
            if (SDK_INT >= ANDROID_R) {
                jitCompileMethodR = reinterpret_cast<bool (*)(void*, void*, void*, int, bool)>(
                        getSymCompat(art_lib_path,
                                     "_ZN3art3jit3Jit13CompileMethodEPNS_9ArtMethodEPNS_6ThreadENS_15CompilationKindEb"));
            } else if (SDK_INT == ANDROID_Q) {
                    jitCompileMethodQ = reinterpret_cast<bool (*)(void *, void *, void *, bool,
                            bool)>(getSymCompat(jit_lib_path, "jit_compile_method"));
            } else {
                jitCompileMethod = reinterpret_cast<bool (*)(void *, void *, void *,
                                                             bool)>(getSymCompat(jit_lib_path,
                                                                                 "jit_compile_method"));
            }
            auto jit_load = getSymCompat(jit_lib_path, "jit_load");
            if (jit_load) {
                if (SDK_INT >= ANDROID_Q) {
                    // Android 10：void* jit_load()
                    // Android 11: JitCompilerInterface* jit_load()
                    jitCompilerHandle = reinterpret_cast<void*(*)()>(jit_load)();
                } else {
                    // void* jit_load(bool* generate_debug_info)
                    bool generate_debug_info = false;
                    jitCompilerHandle = reinterpret_cast<void*(*)(void*)>(jit_load)(&generate_debug_info);
                }
            } else {
                jitCompilerHandle = getGlobalJitCompiler();
            }

            if (jitCompilerHandle != nullptr) {
                art::CompilerOptions* compilerOptions = getCompilerOptions(
                        reinterpret_cast<art::jit::JitCompiler *>(jitCompilerHandle));
                disableJitInline(compilerOptions);
            }

        }


        //init suspend
        innerSuspendVM = reinterpret_cast<void (*)()>(getSymCompat(art_lib_path,
                                                                         "_ZN3art3Dbg9SuspendVMEv"));
        innerResumeVM = reinterpret_cast<void (*)()>(getSymCompat(art_lib_path,
                                                                        "_ZN3art3Dbg8ResumeVMEv"));


        //init for getObject & JitCompiler
        const char* add_weak_ref_sym;
        if (SDK_INT < ANDROID_M) {
            add_weak_ref_sym = "_ZN3art9JavaVMExt22AddWeakGlobalReferenceEPNS_6ThreadEPNS_6mirror6ObjectE";
        } else if (SDK_INT < ANDROID_N) {
            add_weak_ref_sym = "_ZN3art9JavaVMExt16AddWeakGlobalRefEPNS_6ThreadEPNS_6mirror6ObjectE";
        } else  {
            add_weak_ref_sym = SDK_INT <= ANDROID_N2
                                           ? "_ZN3art9JavaVMExt16AddWeakGlobalRefEPNS_6ThreadEPNS_6mirror6ObjectE"
                                           : "_ZN3art9JavaVMExt16AddWeakGlobalRefEPNS_6ThreadENS_6ObjPtrINS_6mirror6ObjectEEE";
        }

        addWeakGlobalRef = reinterpret_cast<jobject (*)(JavaVM *, void *,
                                                   void *)>(getSymCompat(art_lib_path, add_weak_ref_sym));

        if (SDK_INT >= ANDROID_Q) {
            origin_jit_update_options = reinterpret_cast<void (**)(void *)>(getSymCompat(art_lib_path, "_ZN3art3jit3Jit20jit_update_options_E"));
        }

        if (SDK_INT > ANDROID_N) {
            profileSaver_ForceProcessProfiles = reinterpret_cast<void (*)()>(getSymCompat(art_lib_path, "_ZN3art12ProfileSaver20ForceProcessProfilesEv"));
        }

        if (SDK_INT >= ANDROID_R) {
            const char *symbol_decode_method = sizeof(void*) == 8 ? "_ZN3art3jni12JniIdManager15DecodeGenericIdINS_9ArtMethodEEEPT_m" : "_ZN3art3jni12JniIdManager15DecodeGenericIdINS_9ArtMethodEEEPT_j";
            void *decodeArtMethod = getSymCompat(art_lib_path, symbol_decode_method);
            if (decodeArtMethod != nullptr) {
                shadowhook_hook_func_addr(decodeArtMethod, reinterpret_cast<void *>(replace_DecodeArtMethodId), (void**) &origin_DecodeArtMethodId);
            }
            void *shouldUseInterpreterEntrypoint = getSymCompat(art_lib_path, "_ZN3art11ClassLinker30ShouldUseInterpreterEntrypointEPNS_9ArtMethodEPKv");
            if (shouldUseInterpreterEntrypoint != nullptr) {
                shadowhook_hook_func_addr(shouldUseInterpreterEntrypoint, reinterpret_cast<void *>(replace_ShouldUseInterpreterEntrypoint), (void**) &origin_ShouldUseInterpreterEntrypoint);
            }
            void* createProxyClass = getSymCompat(art_lib_path, "_ZN3art11ClassLinker16CreateProxyClassERNS_33ScopedObjectAccessAlreadyRunnableEP8_jstringP13_jobjectArrayP8_jobjectS6_S6_");
            if (createProxyClass != nullptr) {
                shadowhook_hook_func_addr(createProxyClass, reinterpret_cast<void *>(replace_CreateProxyClass), (void**) &origin_CreateProxyClass);
            }
        }
        runtime_instance_ = *reinterpret_cast<void**>(getSymCompat(art_lib_path, "_ZN3art7Runtime9instance_E"));
    }

    bool canCompile() {
        if (SDK_INT >= ANDROID_R)
            return false;
        if (getGlobalJitCompiler() == nullptr) {
            LOGE("JIT not init!");
            return false;
        }
        JNIEnv *env;
        jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
        return getBooleanFromJava(env, "com/swift/sandhook/SandHookConfig",
                                  "compiler");
    }

    bool compileMethod(void* artMethod, void* thread) {
        if (jitCompilerHandle == nullptr)
            return false;
        if (!canCompile()) return false;

        //backup thread flag and state because of jit compile function will modify thread state
        uint32_t old_flag_and_state = *((uint32_t *) thread);
        bool ret;
        if (SDK_INT >= ANDROID_R) {
            if (jitCompileMethodR == nullptr) {
                return false;
            }
            // 高版本 jit 编译待定，因为偏移不固定
//            void* jit = *reinterpret_cast<void**>(reinterpret_cast<size_t>(runtime_instance_) + 0x210);
//            ret = jitCompileMethodR(jit, artMethod, thread, 1, false);
        } else
            if (SDK_INT == ANDROID_Q) {
            if (jitCompileMethodQ == nullptr) {
                return false;
            }
            ret = jitCompileMethodQ(jitCompilerHandle, artMethod, thread, false, false);
        } else {
            if (jitCompileMethod == nullptr) {
                return false;
            }
            ret= jitCompileMethod(jitCompilerHandle, artMethod, thread, false);
        }
        memcpy(thread, &old_flag_and_state, 4);
        return ret;
    }

    void suspendVM() {
        if (innerSuspendVM == nullptr || innerResumeVM == nullptr)
            return;
        innerSuspendVM();
    }

    void resumeVM() {
        if (innerSuspendVM == nullptr || innerResumeVM == nullptr)
            return;
        innerResumeVM();
    }

    bool canGetObject() {
        return addWeakGlobalRef != nullptr;
    }

    void *getCurrentThread() {
        return __get_tls()[TLS_SLOT_ART_THREAD];
    }

    jobject getJavaObject(JNIEnv* env, void* thread, void* address) {
        if (addWeakGlobalRef == nullptr)
            return nullptr;

        jobject object = addWeakGlobalRef(jvm, thread, address);
        if (object == nullptr)
            return nullptr;

        jobject result = env->NewLocalRef(object);
        env->DeleteWeakGlobalRef(object);

        return result;
    }

    art::jit::JitCompiler* getGlobalJitCompiler() {
        if (SDK_INT < ANDROID_N)
            return nullptr;
        if (globalJitCompileHandlerAddr == nullptr)
            return nullptr;
        return *globalJitCompileHandlerAddr;
    }

    art::CompilerOptions* getCompilerOptions(art::jit::JitCompiler* compiler) {
        if (compiler == nullptr)
            return nullptr;
        return compiler->compilerOptions.get();
    }

    art::CompilerOptions* getGlobalCompilerOptions() {
        return getCompilerOptions(getGlobalJitCompiler());
    }

    bool disableJitInline(art::CompilerOptions* compilerOptions) {
        if (compilerOptions == nullptr)
            return false;
        size_t originOptions = compilerOptions->getInlineMaxCodeUnits();
        //maybe a real inlineMaxCodeUnits
        if (originOptions > 0 && originOptions <= 1024) {
            compilerOptions->setInlineMaxCodeUnits(0);
            return true;
        } else {
            return false;
        }
    }

    void* getInterpreterBridge(bool isNative) {
        SandHook::ElfImg libart(art_lib_path);
        if (isNative) {
            return reinterpret_cast<void *>(libart.getSymbAddress("art_quick_generic_jni_trampoline"));
        } else {
            return reinterpret_cast<void *>(libart.getSymbAddress("art_quick_to_interpreter_bridge"));
        }
    }

    //to replace jit_update_option
    void fake_jit_update_options(void* handle) {
        //do nothing
        LOGW("android q: art request update compiler options");
        return;
    }

    bool replaceUpdateCompilerOptionsQ() {
        if (SDK_INT < ANDROID_Q)
            return false;
        if (origin_jit_update_options == nullptr
            || *origin_jit_update_options == nullptr)
            return false;
        *origin_jit_update_options = fake_jit_update_options;
        return true;
    }

    bool forceProcessProfiles() {
        if (profileSaver_ForceProcessProfiles == nullptr)
            return false;
        profileSaver_ForceProcessProfiles();
        return true;
    }

    void replaceFixupStaticTrampolines(void *thiz, void *clazz_ptr) {
        SHADOWHOOK_STACK_SCOPE();
        SHADOWHOOK_ALLOW_REENTRANT();
        backup_fixup_static_trampolines(thiz, clazz_ptr);
        SHADOWHOOK_DISALLOW_REENTRANT();
        if (class_init_callback) {
            class_init_callback(clazz_ptr);
        }
    }

    void *replaceMarkClassInitialized(void * thiz, void * self, uint32_t * clazz_ptr) {
        SHADOWHOOK_STACK_SCOPE();
        SHADOWHOOK_ALLOW_REENTRANT();
        auto result = backup_mark_class_initialized(thiz, self, clazz_ptr);
        SHADOWHOOK_DISALLOW_REENTRANT();
        if (class_init_callback) {
            class_init_callback(reinterpret_cast<void*>(*clazz_ptr));
        }
        return result;
    }

    void replaceUpdateMethodsCode(void *thiz, ArtMethod * artMethod, const void *quick_code) {
        SHADOWHOOK_STACK_SCOPE();
        if (SandHook::TrampolineManager::get().methodHooked(artMethod)) {
            return; //skip
        }
        SHADOWHOOK_ALLOW_REENTRANT();
        backup_update_methods_code(thiz, artMethod, quick_code);
        SHADOWHOOK_DISALLOW_REENTRANT();
    }

void MakeInitializedClassesVisiblyInitialized(void* self){
    if (make_initialized_classes_visibly_initialized_) {
        if (class_linker_ == nullptr) {
            getClassLinker(attachAndGetEvn());
        }

        if (class_linker_ != nullptr) {
            make_initialized_classes_visibly_initialized_(class_linker_, self, true);
        } else {
            LOGW("class_linker_ is null");
        }
    }
}

    bool hookClassInit(void(*callback)(void*)) {
        if (SDK_INT >= ANDROID_R) {
            void *symMarkClassInitialized = getSymCompat(art_lib_path,
                                                           "_ZN3art11ClassLinker20MarkClassInitializedEPNS_6ThreadENS_6HandleINS_6mirror5ClassEEE");
            if (symMarkClassInitialized == nullptr || shadowhook_hook_func_addr == nullptr)
                return false;

            void *symUpdateMethodsCode = getSymCompat(art_lib_path,
                                                         "_ZN3art15instrumentation15Instrumentation21UpdateMethodsCodeImplEPNS_9ArtMethodEPKv");
            if (symUpdateMethodsCode == nullptr || shadowhook_hook_func_addr == nullptr)
                return false;

            shadowhook_hook_func_addr(symMarkClassInitialized, reinterpret_cast<void *>(replaceMarkClassInitialized), (void**) &backup_mark_class_initialized);

            shadowhook_hook_func_addr(symUpdateMethodsCode, reinterpret_cast<void *>(replaceUpdateMethodsCode), (void**) &backup_update_methods_code);

            make_initialized_classes_visibly_initialized_ = reinterpret_cast<void* (*)(void*, void*, bool)>(
                    getSymCompat(art_lib_path, "_ZN3art11ClassLinker40MakeInitializedClassesVisiblyInitializedEPNS_6ThreadEb"));

            if (backup_mark_class_initialized && backup_update_methods_code) {
                class_init_callback = callback;
                return true;
            } else {
                return false;
            }
        } else {
            void *symFixupStaticTrampolines = getSymCompat(art_lib_path,
                                                           "_ZN3art11ClassLinker22FixupStaticTrampolinesENS_6ObjPtrINS_6mirror5ClassEEE");

            if (symFixupStaticTrampolines == nullptr) {
                //huawei lon-al00 android 7.0 api level 24
                symFixupStaticTrampolines = getSymCompat(art_lib_path,
                                                         "_ZN3art11ClassLinker22FixupStaticTrampolinesEPNS_6mirror5ClassE");
            }
            if (symFixupStaticTrampolines == nullptr || shadowhook_hook_func_addr == nullptr)
                return false;

            shadowhook_hook_func_addr(symFixupStaticTrampolines, reinterpret_cast<void *>(replaceFixupStaticTrampolines), (void**) &backup_fixup_static_trampolines);

            if (backup_fixup_static_trampolines) {
                class_init_callback = callback;
                return true;
            } else {
                return false;
            }
        }
    }

    JNIEnv *getEnv() {
        JNIEnv *env;
        jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
        return env;
    }

    JNIEnv *attachAndGetEvn() {
        JNIEnv *env = getEnv();
        if (env == nullptr) {
            jvm->AttachCurrentThread(&env, nullptr);
        }
        return env;
    }

    static bool isIndexId(jmethodID mid) {
        return (reinterpret_cast<uintptr_t>(mid) % 2) != 0;
    }

    ArtMethod* getArtMethod(JNIEnv *env, jobject method) {
        jmethodID methodId = env->FromReflectedMethod(method);
        if (SDK_INT >= ANDROID_R && isIndexId(methodId)) {
            if (origin_DecodeArtMethodId == nullptr || jniIdManager == nullptr) {
                auto res = callStaticMethodAddr(env, "com/swift/sandhook/SandHook", "getArtMethod",
                                                "(Ljava/lang/reflect/Member;)J", method);
                return reinterpret_cast<ArtMethod *>(res);
            } else {
                return origin_DecodeArtMethodId(jniIdManager, methodId);
            }
        } else {
            return reinterpret_cast<ArtMethod *>(methodId);
        }
    }

}