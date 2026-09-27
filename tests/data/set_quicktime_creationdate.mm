// 試験用の MOV に QuickTime 形式の作成日時（mdta/com.apple.quicktime.creationdate）を書く。
// ffmpeg の -metadata では iTunes 形式の欄に入ってしまい、iPhone などの MOV と同じ形にならないため。
//   clang++ -fobjc-arc set_quicktime_creationdate.mm -framework AVFoundation -framework CoreMedia -framework Foundation
//   ./a.out 入力.mov 出力.mov "2024-01-19T02:24:06.250+0900"
#import <AVFoundation/AVFoundation.h>
#include <cstdio>
int main(int argc, char** argv) {
  @autoreleasepool {
    AVURLAsset* a = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]] options:nil];
    AVAssetExportSession* e = [AVAssetExportSession exportSessionWithAsset:a presetName:AVAssetExportPresetPassthrough];
    e.outputURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[2]]];
    e.outputFileType = AVFileTypeQuickTimeMovie;
    AVMutableMetadataItem* m = [AVMutableMetadataItem metadataItem];
    m.identifier = AVMetadataIdentifierQuickTimeMetadataCreationDate;
    m.value = [NSString stringWithUTF8String:argv[3]];
    e.metadata = @[m];
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [e exportAsynchronouslyWithCompletionHandler:^{ dispatch_semaphore_signal(sem); }];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    printf("status %ld %s\n", (long)e.status, [[e.error description] UTF8String]);
  }
}
