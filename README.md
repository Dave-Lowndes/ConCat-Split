# ConCat/Split
This repository contains the core source code for <a href="https://www.jddesign.co.uk/products/concat/concat-s.htm" target="_blank">ConCat/Split</a>, which is now freely available from that web site.

Some of the references are to sources in other private projects, so this solution won't build as it stands - though these are only periphery functions. All the core code of the shell extension, user interface, and functionality are here.

This latest version greatly simplifies the code from the prior release (from 2012) by using more modern C++ features and libraries.

Originally this was a plain Win32 SDK project, but this latest version uses <a href="https://docs.microsoft.com/en-us/cpp/mfc/mfc-desktop-applications" target="_blank">MFC</a> for the user interface dialog code.

I build this code with Visual Studio 2022, and use Visual Studio's static code analysis, and the excellent <a href="https://pvs-studio.com/" target="_blank">PVS-Studio</a> tool to ensure the inevitable silly mistakes we all make are found (*and fixed*) as soon as possible.