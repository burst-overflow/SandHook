package com.swift.sandhook;

import java.lang.reflect.Proxy;

public class ClassLinkerHelper {
    private interface Placeholder0 {
        void test();
    }
    private static class Placeholder1 implements Placeholder0 {

        @Override
        public void test() {

        }
    }


    public static void GetClassLinker() {
        // TODO: 用于触发 ClassLinker 方法 CreateProxyClass，触发后可以获取到本进程的 class_linker_ 对象
        // 此函数主要服务与 MakeInitializedClassesVisiblyInitialized, 解决获取偏移的问题
        Proxy.newProxyInstance(Placeholder1.class.getClassLoader(),
                Placeholder1.class.getInterfaces(), (proxy, method, args) -> null);
    }
}
