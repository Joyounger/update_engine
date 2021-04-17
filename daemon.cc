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
  binder_service_ = new BinderUpdateEngineAndroidService{
      daemon_state_android->service_delegate()};
#endif  // USE_OMAHA
  auto binder_wrapper = android::BinderWrapper::Get();
  if (!binder_wrapper->RegisterService(binder_service_->ServiceName(),
                                       binder_service_)) {
    LOG(ERROR) << "Failed to register binder service.";
  }

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
  daemon_state_->StartUpdater();
#endif  // USE_DBUS
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
