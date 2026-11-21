on run argv
    set rustKernel to item 2 of argv
    set tccKernel to item 3 of argv
    set kernels to {rustKernel}
    set hint to "TCC 启动盘尚未就绪。制盘方法见 Kernel/native/README.md。"
    if item 1 of argv is "true" then
        set kernels to {rustKernel, tccKernel}
        set hint to "请选择本次启动的内核。切换前请在当前系统输入 q 并回车关机。"
    end if
    set startButton to "启动"
    if (count of argv) ≥ 4 then
        set localDisk to item 4 of argv
        if localDisk is not "" then
            set hint to "共享盘 shared.img 仍在 iCloud 中。本次可启动本地桌面盘 " & localDisk & "，其中的文件和历史独立保存。要使用共享盘，请取消并在 Finder 下载 shared.img。"
            set startButton to "启动本地桌面盘"
        end if
    end if
    if (count of argv) ≥ 5 then
        if item 5 of argv is "daily" then set hint to "我的工作盘 · daily.img（Mac 本地）。两个内核共用文件和命令历史。"
    end if
    set preferred to rustKernel
    if (count of argv) ≥ 6 and (count of kernels) is 2 then
        if item 6 of argv is "tcc" then set preferred to tccKernel
    end if
    activate
    set picked to choose from list kernels with title "LixinOS 启动" with prompt hint default items {preferred} OK button name startButton cancel button name "取消"
    if picked is false then return "cancel"
    if item 1 of picked is tccKernel then return "tcc"
    return "rust"
end run
