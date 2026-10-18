#include "android_quest.h"

#include "goemon_config.h"
#include "librecomp/game.hpp"

#include <SDL.h>

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <jni.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <sys/system_properties.h>

namespace {

bool copy_asset_file(AAssetManager* manager, const std::string& asset_path, const std::filesystem::path& dest) {
    AAsset* asset = AAssetManager_open(manager, asset_path.c_str(), AASSET_MODE_STREAMING);
    if (asset == nullptr) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
    std::ofstream out(dest, std::ios::binary);
    if (!out) {
        AAsset_close(asset);
        return false;
    }
    char buffer[8192];
    int read = 0;
    while ((read = AAsset_read(asset, buffer, sizeof(buffer))) > 0) {
        out.write(buffer, read);
    }
    AAsset_close(asset);
    return out.good();
}

void copy_asset_tree(JNIEnv* env, jobject asset_manager, jmethodID list_method, AAssetManager* manager, const std::string& asset_path, const std::filesystem::path& dest) {
    jstring jpath = env->NewStringUTF(asset_path.c_str());
    jobjectArray names = static_cast<jobjectArray>(env->CallObjectMethod(asset_manager, list_method, jpath));
    env->DeleteLocalRef(jpath);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        names = nullptr;
    }

    const jsize count = names != nullptr ? env->GetArrayLength(names) : 0;
    if (count == 0) {
        if (names != nullptr) {
            env->DeleteLocalRef(names);
        }
        AAsset* asset = AAssetManager_open(manager, asset_path.c_str(), AASSET_MODE_STREAMING);
        if (asset != nullptr) {
            AAsset_close(asset);
            copy_asset_file(manager, asset_path, dest);
        } else {
            std::error_code ec;
            std::filesystem::create_directories(dest, ec);
        }
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(dest, ec);
    for (jsize i = 0; i < count; ++i) {
        jstring jname = static_cast<jstring>(env->GetObjectArrayElement(names, i));
        const char* name = env->GetStringUTFChars(jname, nullptr);
        const std::string child = asset_path.empty() ? std::string(name) : asset_path + "/" + name;
        const std::filesystem::path child_dest = dest / name;
        env->ReleaseStringUTFChars(jname, name);
        env->DeleteLocalRef(jname);
        copy_asset_tree(env, asset_manager, list_method, manager, child, child_dest);
    }
    env->DeleteLocalRef(names);
}

} // namespace

void goemon_android_prepare_storage() {
    const char* storage = SDL_AndroidGetExternalStoragePath();
    if (storage == nullptr) {
        std::fprintf(stderr, "Quest: external storage is unavailable.\n");
        return;
    }

    const std::filesystem::path dest = std::filesystem::path(storage) / std::filesystem::path(goemon64::program_id);
    std::error_code ec;
    std::filesystem::create_directories(dest, ec);
    std::filesystem::current_path(dest, ec);
    if (ec) {
        std::fprintf(stderr, "Quest: could not enter %s\n", dest.string().c_str());
        return;
    }

    const std::filesystem::path stamp = dest / ".assets_extracted_v1";
    if (!std::filesystem::exists(stamp)) {
        JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
        jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
        if (env != nullptr && activity != nullptr) {
            jclass activity_class = env->GetObjectClass(activity);
            jmethodID get_assets = env->GetMethodID(activity_class, "getAssets", "()Landroid/content/res/AssetManager;");
            jobject java_assets = env->CallObjectMethod(activity, get_assets);
            if (env->ExceptionCheck() || java_assets == nullptr) {
                env->ExceptionClear();
                std::fprintf(stderr, "Quest: AssetManager is unavailable. Game files were not extracted.\n");
            } else {
                jclass manager_class = env->GetObjectClass(java_assets);
                jmethodID list_method = env->GetMethodID(manager_class, "list", "(Ljava/lang/String;)[Ljava/lang/String;");
                AAssetManager* manager = AAssetManager_fromJava(env, java_assets);
                if (manager != nullptr && list_method != nullptr) {
                    copy_asset_tree(env, java_assets, list_method, manager, "assets", dest / "assets");
                    copy_asset_file(manager, "recompcontrollerdb.txt", dest / "recompcontrollerdb.txt");
                    std::ofstream stamp_file(stamp, std::ios::binary);
                    stamp_file << "1\n";
                }
                env->DeleteLocalRef(manager_class);
                env->DeleteLocalRef(java_assets);
            }
            env->DeleteLocalRef(activity_class);
        }
        if (activity != nullptr && env != nullptr) {
            env->DeleteLocalRef(activity);
        }
    }

    // goemon64::get_app_folder_path() uses the game folder when portable.txt is there.
    if (!std::filesystem::exists(dest / "portable.txt")) {
        std::ofstream(dest / "portable.txt") << "Quest: settings and saves live in this folder.\n";
    }
    if (!std::filesystem::exists(dest / "mnsg.us.z64")) {
        std::ofstream hint(dest / "PUT_YOUR_ROM_HERE.txt");
        hint << "The game asks for your Mystical Ninja Starring Goemon (USA) ROM when it starts.\n";
        hint << "You can also copy the ROM into this folder with any name (.z64, .n64 or .v64).\n";
    }
}

namespace {

std::string rom_error_text(recomp::RomValidationError error) {
    switch (error) {
    case recomp::RomValidationError::FailedToOpen:
        return "That file couldn't be read. Please choose it again.";
    case recomp::RomValidationError::NotARom:
        return "That file isn't an N64 ROM. Please choose your Mystical Ninja Starring Goemon ROM.";
    case recomp::RomValidationError::IncorrectRom:
        return "That's a different N64 game. Please choose Mystical Ninja Starring Goemon (USA).";
    case recomp::RomValidationError::IncorrectVersion:
        return "That's Mystical Ninja Starring Goemon, but not the USA version this port needs. Please choose the USA ROM.";
    default:
        return "That ROM couldn't be used. Please choose Mystical Ninja Starring Goemon (USA).";
    }
}

bool looks_like_rom_file(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext == ".z64" || ext == ".n64" || ext == ".v64" || ext == ".rom";
}

} // namespace

static std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

// `adb shell setprop debug.goemon.autoplay 1`: test runs without anyone in the
// headset start with the stored ROM, without the window. Like every test
// switch it only counts until debug.goemon.testuntil (seconds since 1970 on
// the headset's clock, set by the test run), so it can't stay on by mistake.
static bool autoplay_test_switch() {
    char until[PROP_VALUE_MAX] = {};
    char value[PROP_VALUE_MAX] = {};
    __system_property_get("debug.goemon.testuntil", until);
    __system_property_get("debug.goemon.autoplay", value);
    const long long until_s = std::atoll(until);
    return until_s > 0 && static_cast<long long>(std::time(nullptr)) < until_s && std::atoi(value) == 1;
}

// Shows the ROM window at every start (RomPicker.java): play with the stored ROM,
// choose another file (any name, .z64/.n64/.v64), or quit. A ROM copied into the
// game folder under its own name is imported first. Returns false to quit.
bool goemon_android_ensure_rom(std::u8string game_id) {
    recomp::check_all_stored_roms();
    const std::filesystem::path folder = std::filesystem::current_path();
    const std::filesystem::path stored = folder / (game_id + u8".z64");
    const std::filesystem::path name_file = folder / "rom_name.txt";
    std::error_code ec;

    if (!recomp::is_rom_valid(game_id)) {
        for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
            if (!entry.is_regular_file(ec) || !looks_like_rom_file(entry.path()) || entry.path() == stored) {
                continue;
            }
            if (recomp::select_rom(entry.path(), game_id) == recomp::RomValidationError::Good) {
                std::fprintf(stdout, "Quest: imported the ROM %s\n", entry.path().filename().string().c_str());
                std::ofstream(name_file, std::ios::binary) << entry.path().filename().string();
                recomp::check_all_stored_roms();
                if (recomp::is_rom_valid(game_id)) {
                    break;
                }
            }
        }
    }

    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (env == nullptr || activity == nullptr) {
        return recomp::is_rom_valid(game_id);
    }
    jclass picker = env->FindClass("com/goemon64/recompiled/RomPicker");
    jmethodID request = picker != nullptr
        ? env->GetStaticMethodID(picker, "request", "(Landroid/app/Activity;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I")
        : nullptr;
    if (picker == nullptr || request == nullptr) {
        env->ExceptionClear();
        env->DeleteLocalRef(activity);
        std::fprintf(stderr, "Quest: the ROM picker is missing from the APK.\n");
        return recomp::is_rom_valid(game_id);
    }

    constexpr jint kQuit = 0;
    constexpr jint kPicked = 1;
    constexpr jint kPlayCurrent = 2;
    const std::filesystem::path picked = folder / "picked_rom.tmp";
    const std::filesystem::path picked_name = folder / "picked_rom.tmp.name";
    std::string problem;
    bool ok = false;
    while (true) {
        const bool have_rom = recomp::is_rom_valid(game_id);
        std::string current;
        if (have_rom) {
            current = read_text_file(name_file);
            if (current.empty()) {
                current = "Mystical Ninja Starring Goemon (USA)";
            }
        }

        if (have_rom && autoplay_test_switch()) {
            std::fprintf(stdout, "Quest: test autoplay, playing the stored ROM without the ROM window.\n");
            ok = true;
            break;
        }

        std::string message = problem.empty() ? "" : problem + "\n\n";
        if (have_rom) {
            message += "Current ROM: " + current + "\n\n"
                "Press Play to start, or choose a different ROM file.";
        }
        else {
            message += "To play, choose your own backup of Mystical Ninja Starring Goemon (USA).\n\n"
                "The file can have any name (.z64, .n64 or .v64). It's copied into the game.";
        }

        jstring jmessage = env->NewStringUTF(message.c_str());
        jstring jpath = env->NewStringUTF(picked.string().c_str());
        jstring jcurrent = have_rom ? env->NewStringUTF(current.c_str()) : nullptr;
        const jint choice = env->CallStaticIntMethod(picker, request, activity, jmessage, jpath, jcurrent);
        env->DeleteLocalRef(jmessage);
        env->DeleteLocalRef(jpath);
        if (jcurrent != nullptr) {
            env->DeleteLocalRef(jcurrent);
        }
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            break;
        }

        if (choice == kQuit) {
            std::fprintf(stdout, "Quest: quit from the ROM window.\n");
            break;
        }
        if (choice == kPlayCurrent && have_rom) {
            ok = true;
            break;
        }
        if (choice == kPicked) {
            const recomp::RomValidationError error = recomp::select_rom(picked, game_id);
            std::filesystem::remove(picked, ec);
            if (error == recomp::RomValidationError::Good) {
                std::filesystem::rename(picked_name, name_file, ec);
                recomp::check_all_stored_roms();
                ok = recomp::is_rom_valid(game_id);
                if (ok) {
                    std::fprintf(stdout, "Quest: the chosen ROM is valid and stored.\n");
                    break;
                }
            }
            std::filesystem::remove(picked_name, ec);
            problem = rom_error_text(error);
        }
    }

    env->DeleteLocalRef(picker);
    env->DeleteLocalRef(activity);
    return ok;
}
