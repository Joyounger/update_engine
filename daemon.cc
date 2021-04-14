//
// Copyright (C) 2015 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "update_engine/daemon.h"

#include <sysexits.h>

#include <base/bind.h>
#include <base/location.h>
#if USE_BINDER
#include <binderwrapper/binder_wrapper.h>
#endif  // USE_BINDER

#if USE_OMAHA
#include "update_engine/real_system_state.h"
#else  // !USE_OMAHA
#include "update_engine/daemon_state_android.h"
#endif  // USE_OMAHA

namespace chromeos_update_engine {

int UpdateEngineDaemon::OnInit() {
  // Register the |subprocess_| singleton with this Daemon as the signal
  // handler.
  // The Subprocess class is a singleton. It's used to spawn off a subprocess
  // and get notified when the subprocess exits.
  // To create the Subprocess singleton just instantiate it with and call Init().
  // You can't have two Subprocess instances initialized at the same time.
  //在common/subprocess.cc 中注册信号处理器
  subprocess_.Init(this);

  // 在Daemon::OnInit中，为SIGTERM，SIGINT注册Daemon::Shutdown，为SIGHUP注册Daemon::Restart
  //调用到external/libbrillo/brillo/daemons/daemon.cc里面，对信号处理器进行初始化
  int exit_code = Daemon::OnInit();
  if (exit_code != EX_OK)
    return exit_code;

// USE_BINDER定义在update_engine.gyp:59:      'USE_BINDER=<(USE_binder)'
// gyp(generate your project)是google的编译工程生产工具，
// 在Android.bp中用"-DUSE_BINDER=1"传入
#if USE_BINDER
  //在 system/core/libbinderwrapper/binder_wrapper.cc 里面创建一个 RealBinderWrapper 对象
  android::BinderWrapper::Create();
  //external/libbrillo/brillo/binder_watcher.cc 当前进程binder 的初始化
  binder_watcher_.Init();
#endif  // USE_BINDER

#if USE_OMAHA
  // Initialize update engine global state but continue if something fails.
  // TODO(deymo): Move the daemon_state_ initialization to a factory method
  // avoiding the explicit re-usage of the |bus| instance, shared between
  // D-Bus service and D-Bus client calls.
  RealSystemState* real_system_state = new RealSystemState();
  daemon_state_.reset(real_system_state);
  LOG_IF(ERROR, !real_system_state->Initialize())
      << "Failed to initialize system state.";
#else  // !USE_OMAHA
  // 初始化全局update engine状态, DaemonStateAndroid继承自DaemonStateInterface
  DaemonStateAndroid* daemon_state_android = new DaemonStateAndroid();
  daemon_state_.reset(daemon_state_android);
  LOG_IF(ERROR, !daemon_state_android->Initialize())
      << "Failed to initialize system state.";
#endif  // USE_OMAHA

#if USE_BINDER
  // Create the Binder Service.
#if USE_OMAHA
  binder_service_ = new BinderUpdateEngineBrilloService{real_system_state};
#else   // !USE_OMAHA
  // 1 service_deletegate返回私有成员update_attempter_
  // 2 BinderUpdateEngineAndroidService的构造函数中将update_attempter_设置给service_delegate_成员
  // 从名字service_delegate_看，这也是一个委托对象。浏览下BinderUpdateEngineAndroidService代码，关于IUpdateEngine接口(包括applyPayload, suspend, resume, cancel, resetStatus)的调用都是直接将其转发给了service_delegate_对象，这意味这所有这些对象最终都是调用update_attemper_的相应操作
  binder_service_ = new BinderUpdateEngineAndroidService{
      daemon_state_android->service_delegate()};
#endif  // USE_OMAHA
  // 注册Update Engine的Binder服务，就是将update_engine的Binder 服务添加到ServiceManager
  // android::BinderWrapper::get() 获取到的是之前创建的RealBinderWrapper对象
  // BinderUpdateEngineAndroidService的服务的注册流程为system/core/libbinderwrapper/real_binder_wrapper.cc：bool RealBinderWrapper::RegisterService
  auto binder_wrapper = android::BinderWrapper::Get();
  if (!binder_wrapper->RegisterService(binder_service_->ServiceName(),
                                       binder_service_)) {
    LOG(ERROR) << "Failed to register binder service.";
  }

  //将binder_service_添加到daemon_state的观察对象中:把传入的observer参数(这里为binder_service_)添加到service_observers_集合中去
  // UpdateAttempterAndroid构造函数中会将这里的daemon_state_传入，它的TerminateUpdateAndNotify，SetStatusAndNotify
  // 这两个函数分别在Update结束和状态更新时对service_observers集合的成员逐个调用SendPayloadApplicationComplete和SendStatusUpdate，目的是向外界发送通知状态更新
  daemon_state_->AddObserver(binder_service_.get());
#endif  // USE_BINDER

#if USE_DBUS
  // Create the DBus service.
  dbus_adaptor_.reset(new UpdateEngineAdaptor(real_system_state));
  daemon_state_->AddObserver(dbus_adaptor_.get());

  dbus_adaptor_->RegisterAsync(base::Bind(&UpdateEngineDaemon::OnDBusRegistered,
                                          base::Unretained(this)));
  LOG(INFO) << "Waiting for DBus object to be registered.";
#else   // !USE_DBUS
  // USE_DBUS默认没定义，之前执行了DaemonStateAndroid* daemon_state_android = new DaemonStateAndroid();daemon_state_.reset(daemon_state_android);
  // 因此调用到bool DaemonStateAndroid::StartUpdater()
  daemon_state_->StartUpdater();
#endif  // USE_DBUS

//1 服务端进程(代码main.cc)在main函数中先解析命令行参数并进行简单初始化，随后创建update_engine_daemon对象，并调用对象的Run()方法进入服务等待状态
//2 在Run()中进入主循环前，通过OnInit()初始化生成两个业务对象binder_service_和daemon_state_，前者负责binder服务对外的工作，后者则负责后台的实际业务
//3 binder_service_在客户端调用bind操作时会保存客户端注册的回调函数，从而在适当的时候通过回调函数告知客户端升级的状态信息；同时binder_service_接收到客户端的服务请求后，将其交给daemon_state_的成员update_attempter_去完成，所以update_attempter_才是Update Engine服务端业务的核心
// 
  return EX_OK;
}

#if USE_DBUS
void UpdateEngineDaemon::OnDBusRegistered(bool succeeded) {
  if (!succeeded) {
    LOG(ERROR) << "Registering the UpdateEngineAdaptor";
    QuitWithExitCode(1);
    return;
  }

  // Take ownership of the service now that everything is initialized. We need
  // to this now and not before to avoid exposing a well known DBus service
  // path that doesn't have the service it is supposed to implement.
  if (!dbus_adaptor_->RequestOwnership()) {
    LOG(ERROR) << "Unable to take ownership of the DBus service, is there "
               << "other update_engine daemon running?";
    QuitWithExitCode(1);
    return;
  }
  daemon_state_->StartUpdater();
}
#endif  // USE_DBUS

}  // namespace chromeos_update_engine
