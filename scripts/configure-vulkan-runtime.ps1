param([string]$VulkanSdk=$env:VULKAN_SDK)
$ErrorActionPreference='Stop'

# 只调整当前进程及其子进程，不修改系统环境变量或注册表。
# 已构建的程序也需要 SDK 的 Layer manifest；只找得到驱动不等于开启了验证层。
$candidates=[Collections.Generic.List[string]]::new()
if($VulkanSdk){$candidates.Add($VulkanSdk)}
foreach($sdkRoot in @('C:\VulkanSDK','D:\develop\VulkanSDK')){
    if(Test-Path -LiteralPath $sdkRoot){
        Get-ChildItem -LiteralPath $sdkRoot -Directory | Sort-Object Name -Descending | ForEach-Object {$candidates.Add($_.FullName)}
    }
}
$sdk=$null
foreach($candidate in $candidates){
    if(Test-Path -LiteralPath (Join-Path $candidate 'Include\vulkan\vulkan.h')){$sdk=$candidate;break}
}
if($sdk){
    $env:VULKAN_SDK=$sdk
    $sdkBin=Join-Path $sdk 'Bin'
    if(@($env:Path -split ';') -notcontains $sdkBin){$env:Path="$sdkBin;$env:Path"}
    if(Test-Path -LiteralPath (Join-Path $sdkBin 'VkLayer_khronos_validation.json')){
        if(@($env:VK_LAYER_PATH -split ';') -notcontains $sdkBin){
            $env:VK_LAYER_PATH=if($env:VK_LAYER_PATH){"$sdkBin;$env:VK_LAYER_PATH"}else{$sdkBin}
        }
    }
}
$manifest=$null
foreach($layerDirectory in @($env:VK_LAYER_PATH -split ';')){
    if($layerDirectory){
        $path=Join-Path $layerDirectory 'VkLayer_khronos_validation.json'
        if(Test-Path -LiteralPath $path){$manifest=$path;break}
    }
}
# 配置成功不等于加载成功；导出与回归脚本还检查实际子进程的 enabled 日志。
[pscustomobject]@{sdk=$sdk;layer_manifest=$manifest;validation_configured=[bool]$manifest}
