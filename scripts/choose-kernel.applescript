on run argv
    set rustKernel to "Rust 原版内核"
    set tccKernel to "TCC 自举内核（C / 汇编）"
    set kernels to {rustKernel}
    set hint to "TCC 启动盘尚未就绪。制盘方法见 Kernel/native/README.md。"
    if item 1 of argv is "true" then
        set kernels to {rustKernel, tccKernel}
        set hint to "请选择本次启动的内核。切换前请在当前系统输入 q 并回车关机。"
    end if
    activate
    set picked to choose from list kernels with title "LixinOS 启动" with prompt hint default items {rustKernel} OK button name "启动" cancel button name "取消"
    if picked is false then return "cancel"
    if item 1 of picked is tccKernel then return "tcc"
    return "rust"
end run
