plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.vi.phoneclient"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.vi.phoneclient"
        minSdk = 26
        targetSdk = 35
        versionCode = 2
        versionName = "2.0"
    }

    signingConfigs {
        create("release") {
            storeFile = file("../release.keystore")
            storePassword = "vi-voice-2026"
            keyAlias = "vi-voice"
            keyPassword = "vi-voice-2026"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            signingConfig = signingConfigs.getByName("release")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}
