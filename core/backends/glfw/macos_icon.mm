#ifdef __APPLE__
#import <Cocoa/Cocoa.h>
#include <string>

void macos_setDockIcon(const std::string& pngPath) {
    @autoreleasepool {
        NSString* path = [NSString stringWithUTF8String:pngPath.c_str()];
        NSImage* icon = [[NSImage alloc] initWithContentsOfFile:path];
        if (icon) {
            [NSApp setApplicationIconImage:icon];
        }
    }
}
#endif
