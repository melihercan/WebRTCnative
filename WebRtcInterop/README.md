# WebRtcInterop

## CLI build
gn gen out/Default --args="is_debug=false is_component_build=true rtc_enable_symbol_export=true rtc_include_tests=false rtc_build_tools=false rtc_build_examples=false"


## VS2022 build
gn gen out/vs2022 --args="is_debug=false is_component_build=true rtc_enable_symbol_export=true rtc_include_tests=false rtc_build_tools=false rtc_build_examples=false" --ide=vs2022
       

## BUILD EXAMPLES
https://github.com/webrtc-sdk/libwebrtc