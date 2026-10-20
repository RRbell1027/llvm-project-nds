// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fopenmp --offload-targets=arm-nds-linux-gnueabihf -fsyntax-only %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fopenmp --offload-targets=riscv64-nds-linux-gnu -fsyntax-only %s
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -fopenmp --offload-targets=arm-unknown-linux-gnueabihf -fsyntax-only %s 2>&1 | FileCheck %s --check-prefix=ARM
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -fopenmp --offload-targets=riscv64-unknown-linux-gnu -fsyntax-only %s 2>&1 | FileCheck %s --check-prefix=RISCV
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -fopenmp --offload-targets=unknown-nds-linux-gnu -fsyntax-only %s 2>&1 | FileCheck %s --check-prefix=UNKNOWN
// ARM: error: OpenMP target is invalid: 'arm-unknown-linux-gnueabihf'
// RISCV: error: OpenMP target is invalid: 'riscv64-unknown-linux-gnu'
// UNKNOWN: error: OpenMP target is invalid: 'unknown-nds-linux-gnu'
