# Win2DViewer
Use Win2D WinRT components with C++/WinRT in a classic multi-window Win32 desktop application.

The application supports loading SVG files and rendering them with Win2D, including robust SVG parsing and rendering behavior for desktop scenarios.

This sample shows how to integrate Win2D WinRT components into a classic C++ desktop app using C++/WinRT.

#### Tools:
Microsoft Visual Studio 2022 (v143 toolset)  
Windows Target Platform Min Version: 10.0.17763.0  
Windows SDK (installed): 10.0.26100.x  
Microsoft Visual C++ Redistributable 2015-2022 (v14.x)  

#### Nuget packages:
Microsoft.UI.Xaml 2.8.7  
Microsoft.VCRTForwarders.140 1.1.0  
Microsoft.Windows.CppWinRT 2.0.250303.1  
Microsoft.Web.WebView2 1.0.2849.39  
Win2D.uwp 1.28.3  

#### OS
Windows 10 Version 1809 or later (build 17763+)

#### Build cleanup script
Use `scripts/Clean-BuildArtifacts.ps1` to safely clean generated outputs.
The script also prunes empty intermediate parent folders such as `x64` and `Win2DViewer\x64`.

Preview only:
```powershell
pwsh -NoProfile -File .\scripts\Clean-BuildArtifacts.ps1 -Configuration All -Platform All -WhatIf
```

Clean `x64 Debug/Release` outputs:
```powershell
pwsh -NoProfile -File .\scripts\Clean-BuildArtifacts.ps1 -Configuration All -Platform x64
```

Also clean `.vs` cache and `*.binlog`:
```powershell
pwsh -NoProfile -File .\scripts\Clean-BuildArtifacts.ps1 -Configuration All -Platform All -IncludeSolutionCache -IncludeBinLogs
```

![](Win2DViewer.gif)


