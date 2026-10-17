// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// JNI surface for the patch-domain C ABI. Compiled both by the Android NDK (bachata_patch_domain
// shared library) and by the host test build (so a JVM unit test can drive the real resolver).
// Every function returns the JSON produced by the C ABI as a jstring; the native string is
// released immediately.

#include <jni.h>

#include "common/patch_domain.h"

namespace {

jstring ResultString(JNIEnv* env, const char* result) {
    jstring out = env->NewStringUTF(result ? result : "{}");
    pd_free_string(result);
    return out;
}

} // namespace

extern "C" {

#define PD_JNI(name)                                                                               \
    JNIEXPORT jstring JNICALL Java_com_bachatas4_android_data_patch_NativePatchDomainBridge_##name

PD_JNI(nativeLoadManifest)(JNIEnv* env, jclass /*clazz*/, jstring repository_root) {
    const char* root = repository_root ? env->GetStringUTFChars(repository_root, nullptr) : nullptr;
    const char* result = pd_load_manifest(root ? root : "");
    if (root) {
        env->ReleaseStringUTFChars(repository_root, root);
    }
    return ResultString(env, result);
}

PD_JNI(nativeLoadState)(JNIEnv* env, jclass /*clazz*/, jstring state_path) {
    const char* path = state_path ? env->GetStringUTFChars(state_path, nullptr) : nullptr;
    const char* result = pd_load_state(path ? path : "");
    if (path) {
        env->ReleaseStringUTFChars(state_path, path);
    }
    return ResultString(env, result);
}

PD_JNI(nativeSaveState)(JNIEnv* env, jclass /*clazz*/, jstring state_path, jstring state_json) {
    const char* path = state_path ? env->GetStringUTFChars(state_path, nullptr) : nullptr;
    const char* state =
        state_json ? env->GetStringUTFChars(state_json, nullptr) : nullptr;
    const char* result = pd_save_state(path ? path : "", state ? state : "");
    if (path) {
        env->ReleaseStringUTFChars(state_path, path);
    }
    if (state) {
        env->ReleaseStringUTFChars(state_json, state);
    }
    return ResultString(env, result);
}

PD_JNI(nativeDefaultState)(JNIEnv* env, jclass /*clazz*/, jstring serial, jstring repository_id) {
    const char* c_serial = serial ? env->GetStringUTFChars(serial, nullptr) : nullptr;
    const char* c_repo = repository_id ? env->GetStringUTFChars(repository_id, nullptr) : nullptr;
    const char* result = pd_default_state(c_serial ? c_serial : "", c_repo ? c_repo : "");
    if (c_serial) {
        env->ReleaseStringUTFChars(serial, c_serial);
    }
    if (c_repo) {
        env->ReleaseStringUTFChars(repository_id, c_repo);
    }
    return ResultString(env, result);
}

PD_JNI(nativeResolveEffective)(JNIEnv* env, jclass /*clazz*/, jstring repository_root,
                               jstring serial, jstring app_version, jstring state_json) {
    const char* root = repository_root ? env->GetStringUTFChars(repository_root, nullptr) : nullptr;
    const char* c_serial = serial ? env->GetStringUTFChars(serial, nullptr) : nullptr;
    const char* c_ver = app_version ? env->GetStringUTFChars(app_version, nullptr) : nullptr;
    const char* c_state = state_json ? env->GetStringUTFChars(state_json, nullptr) : nullptr;
    const char* result = pd_resolve_effective(root ? root : "", c_serial ? c_serial : "",
                                              c_ver ? c_ver : "", c_state);
    if (root) {
        env->ReleaseStringUTFChars(repository_root, root);
    }
    if (c_serial) {
        env->ReleaseStringUTFChars(serial, c_serial);
    }
    if (c_ver) {
        env->ReleaseStringUTFChars(app_version, c_ver);
    }
    if (c_state) {
        env->ReleaseStringUTFChars(state_json, c_state);
    }
    return ResultString(env, result);
}

PD_JNI(nativeBuildSession)(JNIEnv* env, jclass /*clazz*/, jstring repository_root, jstring serial,
                           jstring app_version, jstring state_json) {
    const char* root = repository_root ? env->GetStringUTFChars(repository_root, nullptr) : nullptr;
    const char* c_serial = serial ? env->GetStringUTFChars(serial, nullptr) : nullptr;
    const char* c_ver = app_version ? env->GetStringUTFChars(app_version, nullptr) : nullptr;
    const char* c_state = state_json ? env->GetStringUTFChars(state_json, nullptr) : nullptr;
    const char* result = pd_build_session(root ? root : "", c_serial ? c_serial : "",
                                          c_ver ? c_ver : "", c_state);
    if (root) {
        env->ReleaseStringUTFChars(repository_root, root);
    }
    if (c_serial) {
        env->ReleaseStringUTFChars(serial, c_serial);
    }
    if (c_ver) {
        env->ReleaseStringUTFChars(app_version, c_ver);
    }
    if (c_state) {
        env->ReleaseStringUTFChars(state_json, c_state);
    }
    return ResultString(env, result);
}

} // extern "C"
