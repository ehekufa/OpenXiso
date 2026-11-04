# Правила для release-сборки с JNI.
# Имена native-методов не должны обфусцироваться: JNI ищет их по сигнатуре
# Java_com_openxiso_app_NativeBridge_*, заданной в openxiso_jni.cpp.
-keepclasseswithmembernames class * {
    native <methods>;
}
-keep class com.openxiso.app.NativeBridge { *; }
