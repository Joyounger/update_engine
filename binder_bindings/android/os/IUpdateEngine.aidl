/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package android.os;

import android.os.IUpdateEngineCallback;

// IUpdateEngine.aidl定义了一个IUpdateEngine接口
// 编译后在out/.../EXECUTABLES/update_engine_client_intermediates/aidl-generated/下会生成
|-- include
|   `-- android
|       `-- os
|           |-- BnUpdateEngine.h
|           |-- BnUpdateEngineCallback.h
|           |-- BpUpdateEngine.h
|           |-- BpUpdateEngineCallback.h
|           |-- IUpdateEngine.h
|           `-- IUpdateEngineCallback.h
`-- src
    `-- binder_bindings
        `-- android
            `-- os
                |-- IUpdateEngine.cc
                |-- IUpdateEngine.o
                |-- IUpdateEngineCallback.cc
                `-- IUpdateEngineCallback.o
————————————————
`IUpdateEngine.aidl`生成了
  --> IUpdateEngine.h, IUpdateEngine.cc
  --> BnUpdateEngine.h
  --> BpUpdateEngine.h

`IUpdateEngineCallback.aidl`生成了
  --> IUpdateEngineCallback.h, IUpdateEngineCallback.cc
  --> BnUpdateEngineCallback.h
  --> BpUpdateEngineCallback.h
————————————————
简单说来，会生成一个IUpdateEngine.h的接口类定义文件，然后再分别生成两个Binder的Native和Proxy相关的类文件BnUpdateEngine.h和BpUpdateEngine.h，这两个文件分别用于实现Bind的Native端接口和Proxy端接口
这3个类的继承定义如下：
class IUpdateEngine : public ::android::IInterface
class BpUpdateEngine : public ::android::BpInterface<IUpdateEngine>
class BnUpdateEngine : public ::android::BnInterface<IUpdateEngine>
这里BpUpdateEngine和BnUpdateEngine分别是继承自BpInterface和BnInterface的模板类，模板数据类型是IUpdateEngine；
实际上这里IUpdateEngine定义了整个服务的接口，BnUpdateEnging和BpUpdateEngine通过模板类的方式，支持所有IUpdateEngine的操作。
通过搜索，可以看到整个update_engine文件夹没有对BpUpdateEngine的引用
grep -rwns "android::os::BnUpdateEngine"
grep -rwns "android::os::BpUpdateEngine"  
grep -rwns "android::os::IUpdateEngine"

/** @hide */
interface IUpdateEngine {
  /** @hide */
  void applyPayload(String url,
                    in long payload_offset,
                    in long payload_size,
                    in String[] headerKeyValuePairs);
  /** @hide */
  boolean bind(IUpdateEngineCallback callback);
  /** @hide */
  boolean unbind(IUpdateEngineCallback callback);
  /** @hide */
  void suspend();
  /** @hide */
  void resume();
  /** @hide */
  void cancel();
  /** @hide */
  void resetStatus();
  /** @hide */
  boolean verifyPayloadApplicable(in String metadataFilename);
}
