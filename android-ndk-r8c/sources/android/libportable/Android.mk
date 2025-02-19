#
# Copyright (C) 2012 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

LOCAL_PATH := $(call my-dir)

#=====================================================================
# Device Shared Library libportable
#=====================================================================

include $(CLEAR_VARS)

LOCAL_MODULE := libportable
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_CLASS := SHARED_LIBRARIES

LOCAL_CFLAGS := -I $(LOCAL_PATH)/common/include

ifeq ($(TARGET_ARCH),mips)
libportable_arch_src_files += \
			arch-mips/clone.c \
			arch-mips/epoll.c \
			arch-mips/errno.c \
			arch-mips/fcntl.c \
			arch-mips/ioctl.c \
			arch-mips/mmap.c \
			arch-mips/open.c \
			arch-mips/poll.c \
			arch-mips/pthread.c \
			arch-mips/resource.c \
			arch-mips/signal.c \
			arch-mips/socket.c \
			arch-mips/sockopt.c \
			arch-mips/stat.c \
			arch-mips/statfs.c \
			arch-mips/syscall.c \
			arch-mips/timer.c \
			arch-mips/waitpid.c
endif

ifeq ($(TARGET_ARCH),arm)
libportable_arch_src_files += \
			arch-arm/epoll.c \
			arch-arm/errno.c \
			arch-arm/socket.c \
			arch-arm/sockopt.c \
			arch-arm/stat.c
endif

ifeq ($(TARGET_ARCH),x86)
libportable_arch_src_files += \
			arch-x86/epoll.c \
			arch-x86/errno.c \
			arch-x86/fcntl.c \
			arch-x86/ioctl.c \
			arch-x86/open.c \
			arch-x86/socket.c \
			arch-x86/sockopt.c \
			arch-x86/stat.c
endif

LOCAL_SRC_FILES := \
        $(libportable_common_src_files) \
        $(libportable_arch_src_files)

LOCAL_SHARED_LIBRARIES += liblog

include $(BUILD_SHARED_LIBRARY)
