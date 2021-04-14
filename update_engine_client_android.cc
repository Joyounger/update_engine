//
// Copyright (C) 2016 The Android Open Source Project
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

#include <sysexits.h>
#include <unistd.h>

#include <string>
#include <vector>

#include <base/bind.h>
#include <base/callback.h>
#include <base/command_line.h>
#include <base/logging.h>
#include <base/strings/string_split.h>
#include <binder/IServiceManager.h>
#include <binderwrapper/binder_wrapper.h>
#include <brillo/binder_watcher.h>
#include <brillo/daemons/daemon.h>
#include <brillo/flag_helper.h>
#include <brillo/message_loops/message_loop.h>
#include <brillo/syslog_logging.h>
#include <utils/String16.h>
#include <utils/StrongPointer.h>

#include "android/os/BnUpdateEngineCallback.h"
#include "android/os/IUpdateEngine.h"
#include "update_engine/client_library/include/update_engine/update_status.h"
#include "update_engine/common/error_code.h"
#include "update_engine/common/error_code_utils.h"
#include "update_engine/update_status_utils.h"

using android::binder::Status;

namespace chromeos_update_engine {
namespace internal {

class UpdateEngineClientAndroid : public brillo::Daemon {
 public:
  // 用传入的参数argc, argv初始化私有成员变量argc_, argv_
  UpdateEngineClientAndroid(int argc, char** argv) : argc_(argc), argv_(argv) {}

  int ExitWhenIdle(const Status& status);
  int ExitWhenIdle(int return_code);

 private:
  class UECallback : public android::os::BnUpdateEngineCallback {
   public:
    explicit UECallback(UpdateEngineClientAndroid* client) : client_(client) {}

    // android::os::BnUpdateEngineCallback overrides.
    Status onStatusUpdate(int status_code, float progress) override;
    Status onPayloadApplicationComplete(int error_code) override;

   private:
    UpdateEngineClientAndroid* client_;
  };

  int OnInit() override;

  // Called whenever the UpdateEngine daemon dies.
  void UpdateEngineServiceDied();

  // 下面定义了私有成员变量argc_和argv_用于存放main函数接收到的参数
  // Copy of argc and argv passed to main().
  int argc_;
  char** argv_;

  android::sp<android::os::IUpdateEngine> service_;
  android::sp<android::os::BnUpdateEngineCallback> callback_;

  brillo::BinderWatcher binder_watcher_;
};

Status UpdateEngineClientAndroid::UECallback::onStatusUpdate(int status_code,
                                                             float progress) {
  update_engine::UpdateStatus status =
      static_cast<update_engine::UpdateStatus>(status_code);
  LOG(INFO) << "onStatusUpdate(" << UpdateStatusToString(status) << " ("
            << status_code << "), " << progress << ")";
  return Status::ok();
}

Status UpdateEngineClientAndroid::UECallback::onPayloadApplicationComplete(
    int error_code) {
  ErrorCode code = static_cast<ErrorCode>(error_code);
  LOG(INFO) << "onPayloadApplicationComplete(" << utils::ErrorCodeToString(code)
            << " (" << error_code << "))";
  client_->ExitWhenIdle(
      (code == ErrorCode::kSuccess || code == ErrorCode::kUpdatedButNotActive)
          ? EX_OK
          : 1);
  return Status::ok();
}

int UpdateEngineClientAndroid::OnInit() {
  // 这里在子类中调用父类的OnInit操作，注册信号SIGTERM, SIGINT和SIGHUP的处理函数
  int ret = Daemon::OnInit();
  if (ret != EX_OK)
    return ret;

  // 命令行处理选项将宏DEFINE_xxx展开，最终得到FLAGS_xxx变量，因此命令行选项和生成的FLAGS_xxx变量的对应关系为：xxx - FLAGS_xxx
  DEFINE_bool(update, false, "Start a new update, if no update in progress.");
  DEFINE_string(payload,
                "http://127.0.0.1:8080/payload",
                "The URI to the update payload to use.");
  DEFINE_int64(offset,
               0,
               "The offset in the payload where the CrAU update starts. "
               "Used when --update is passed.");
  DEFINE_int64(size,
               0,
               "The size of the CrAU part of the payload. If 0 is passed, it "
               "will be autodetected. Used when --update is passed.");
  DEFINE_string(headers,
                "",
                "A list of key-value pairs, one element of the list per line. "
                "Used when --update is passed.");

  DEFINE_bool(verify,
              false,
              "Given payload metadata, verify if the payload is applicable.");
  DEFINE_string(metadata,
                "/data/ota_package/metadata",
                "The path to the update payload metadata. "
                "Used when --verify is passed.");

  DEFINE_bool(suspend, false, "Suspend an ongoing update and exit.");
  DEFINE_bool(resume, false, "Resume a suspended update.");
  DEFINE_bool(cancel, false, "Cancel the ongoing update and exit.");
  DEFINE_bool(reset_status, false, "Reset an already applied update and exit.");
  DEFINE_bool(follow,
              false,
              "Follow status update changes until a final state is reached. "
              "Exit status is 0 if the update succeeded, and 1 otherwise.");

  // Boilerplate init commands.
  // 用argc_, argv_初始化命令行解析器
  base::CommandLine::Init(argc_, argv_);
  // 在这里解析argc_和argv_参数，如果不带参数，则显示错误并返回
  brillo::FlagHelper::Init(argc_, argv_, "Android Update Engine Client");
  if (argc_ == 1) {
    LOG(ERROR) << "Nothing to do. Run with --help for help.";
    return 1;
  }

  // Ensure there are no positional arguments.
  const std::vector<std::string> positional_args =
      base::CommandLine::ForCurrentProcess()->GetArgs();
  if (!positional_args.empty()) {
    LOG(ERROR) << "Found a positional argument '" << positional_args.front()
               << "'. If you want to pass a value to a flag, pass it as "
                  "--flag=value.";
    return 1;
  }

  bool keep_running = false;
  // 初始化Log操作
  brillo::InitLog(brillo::kLogToStderr);

  // Initialize a binder watcher early in the process before any interaction
  // with the binder driver.
  binder_watcher_.Init();

  // 获取"android.os.UpdateEngineService"服务，并将其代理对象存放到service_中，可以简单理解为所有UpdateEngineService服务的操作都可以调用service_成员的相应方法来实现
  // update_engine\binder_service_android.h中，class BinderUpdateEngineAndroidService有const char* ServiceName() const { return "android.os.UpdateEngineService"; } 
  // 因此getService执行后，service_的方法都调用到class BinderUpdateEngineAndroidService的对应方法
  android::status_t status = android::getService(
      android::String16("android.os.UpdateEngineService"), &service_);
  if (status != android::OK) {
    LOG(ERROR) << "Failed to get IUpdateEngine binder from service manager: "
               << Status::fromStatusT(status).toString8();
    return ExitWhenIdle(1);
  }

/*
命令行调用的操作：
#update_engine_client \
  --payload=http://stbszx-bld-5/public/android/full-ota/payload.bin \
  --update \
  --headers="\
	FILE_HASH=ozGgyQEcnkI5ZaX+Wbjo5I/PCR7PEZka9fGd0nWa+oY= \
	FILE_SIZE=282164983
	METADATA_HASH=GLIKfE6KRwylWMHsNadG/Q8iy5f7ENWTatvMdBlpoPg= \
	METADATA_SIZE=21023 \
  "
因此
FLAGS_payload: "http://stbszx-bld-5/public/android/full-ota/payload.bin"
FLAGS_update: true
FLAGS_headers: "FILE_HASH=ozGgyQEcnkI5ZaX+Wbjo5I/PCR7PEZka9fGd0nWa+oY= \
                FILE_SIZE=282164983
                METADATA_HASH=GLIKfE6KRwylWMHsNadG/Q8iy5f7ENWTatvMdBlpoPg= \
                METADATA_SIZE=21023"

*/

  // 将命令行update_engine_client提供的各种操作，如suspend, resume, cancel, reset_status, follow, update通过代理对象service_通知服务进程UpdateEngineService
  if (FLAGS_suspend) {
    return ExitWhenIdle(service_->suspend());
  }

  if (FLAGS_resume) {
    return ExitWhenIdle(service_->resume());
  }

  if (FLAGS_cancel) {
    return ExitWhenIdle(service_->cancel());
  }

  if (FLAGS_reset_status) {
    return ExitWhenIdle(service_->resetStatus());
  }

  if (FLAGS_verify) {
    bool applicable = false;
    Status status = service_->verifyPayloadApplicable(
        android::String16{FLAGS_metadata.data(), FLAGS_metadata.size()},
        &applicable);
    LOG(INFO) << "Payload is " << (applicable ? "" : "not ") << "applicable.";
    return ExitWhenIdle(status);
  }

  // 如果指定"follow"选项，则绑定回调操作UECallback
  if (FLAGS_follow) {
    // Register a callback object with the service.
    // 生成一个UECallback对象，并通过service_->bind(callback_, &bound)将其绑定到UpdateEngineService服务端的IUpdateEngineCallback对象上
    callback_ = new UECallback(this);
    bool bound;
    if (!service_->bind(callback_, &bound).isOk() || !bound) {
      LOG(ERROR) << "Failed to bind() the UpdateEngine daemon.";
      return 1;
    }
    keep_running = true;
  }

  if (FLAGS_update) {
  	// 解析"headers"，生成键值对列表
  	// 先将FLAGS_headers按照换行符”\n“进行拆分，并存放到headers中
  	// 将headers的每一项通过push_back操作存放到容器and_headers中
    std::vector<std::string> headers = base::SplitString(
        FLAGS_headers, "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
    std::vector<android::String16> and_headers;
    for (const auto& header : headers) {
      and_headers.push_back(android::String16{header.data(), header.size()});
    }
	// 调用服务进程的"applyPlayload"操作
	// 将payload, offset, size参数和解析得到的and_headers一并传递给service_->applyPayload()方法，此时服务端UpdateEngineService进程会调用applyPayload进行升级更新
	// service_->applyPayload直接调用到了Status BinderUpdateEngineAndroidService::applyPayload(
    // const android::String16& url,
    // int64_t payload_offset,
    // int64_t payload_size,
    // const std::vector<android::String16>& header_kv_pairs)
	// 这里最终调用到的是UpdateAttempterAndroid::ApplyPayload
    Status status = service_->applyPayload(
        android::String16{FLAGS_payload.data(), FLAGS_payload.size()},
        FLAGS_offset,
        FLAGS_size,
        and_headers);
    if (!status.isOk())
      return ExitWhenIdle(status);
  }

  // follow状态需要一直跟踪server端的状态，因此要求一直运行，但除follow操作外的其它操作，在执行完后就完成了，不再需要继续执行，所以如果keep_runing为false，则退出
  if (!keep_running)
    return ExitWhenIdle(EX_OK);

  // When following updates status changes, exit if the update_engine daemon
  // dies.
  // 如果server端挂掉了，再follow就没有意义了，所以注册一个事件来检查server端是否已经挂掉
  android::BinderWrapper::Create();
  android::BinderWrapper::Get()->RegisterForDeathNotifications(
      android::os::IUpdateEngine::asBinder(service_),
      base::Bind(&UpdateEngineClientAndroid::UpdateEngineServiceDied,
                 base::Unretained(this)));

  return EX_OK;
}

int UpdateEngineClientAndroid::ExitWhenIdle(const Status& status) {
  if (status.isOk())
    return ExitWhenIdle(EX_OK);
  LOG(ERROR) << status.toString8();
  return ExitWhenIdle(status.exceptionCode());
}

int UpdateEngineClientAndroid::ExitWhenIdle(int return_code) {
  auto delayed_exit = base::Bind(
      &Daemon::QuitWithExitCode, base::Unretained(this), return_code);
  if (!brillo::MessageLoop::current()->PostTask(delayed_exit))
    return 1;
  return EX_OK;
}

void UpdateEngineClientAndroid::UpdateEngineServiceDied() {
  LOG(ERROR) << "UpdateEngineService died.";
  QuitWithExitCode(1);
}

}  // namespace internal
}  // namespace chromeos_update_engine

/* bcm7252ssffdr4:/ # update_engine_client --help 
Android Update Engine Client

  --cancel  (Cancel the ongoing update and exit.)  type: bool  default: false
  --follow  (Follow status update changes until a final state is reached. Exit status is 0 if the update succeeded, and 1 otherwise.)  type: bool  default: false
  --headers  (A list of key-value pairs, one element of the list per line. Used when --update is passed.)  type: string  default: ""
  --help  (Show this help message)  type: bool  default: false
  --offset  (The offset in the payload where the CrAU update starts. Used when --update is passed.)  type: int64  default: 0
  --payload  (The URI to the update payload to use.)  type: string  default: "http://127.0.0.1:8080/payload"
  --reset_status  (Reset an already applied update and exit.)  type: bool  default: false
  --resume  (Resume a suspended update.)  type: bool  default: false
  --size  (The size of the CrAU part of the payload. If 0 is passed, it will be autodetected. Used when --update is passed.)  type: int64  default: 0
  --suspend  (Suspend an ongoing update and exit.)  type: bool  default: false
  --update  (Start a new update, if no update in progress.)  type: bool  default: false
————————————————
*/
// Android自带的客户端demo进程update_engine_client的入口
int main(int argc, char** argv) {
  chromeos_update_engine::internal::UpdateEngineClientAndroid client(argc,
                                                                     argv);
  // 还是先执行external\libbrillo\brillo\daemons\daemon.cc父类brillo::Daemon的Run()函数
  return client.Run();
}
