#import "LSAppSupport.h"

#include <algorithm>
#include <cmath>

#include "stackcore/fits_writer.hpp"
#include "stackcore/png_writer.hpp"
#include "stackcore/tiff_writer.hpp"

@implementation FlippedView
- (BOOL)isFlipped {
    return YES;
}
@end

@implementation VerticalClipView
- (NSRect)constrainBoundsRect:(NSRect)proposedBounds {
    NSRect r = [super constrainBoundsRect:proposedBounds];
    r.origin.x = 0.0;
    return r;
}

// スクロールの経路によっては constrainBoundsRect: を通らずに原点が設定されるので、
// 原点を動かす入口でも x を 0 に戻す。
- (void)scrollToPoint:(NSPoint)newOrigin {
    newOrigin.x = 0.0;
    [super scrollToPoint:newOrigin];
}

- (void)setBoundsOrigin:(NSPoint)newOrigin {
    newOrigin.x = 0.0;
    [super setBoundsOrigin:newOrigin];
}
@end


void write_output_image(const std::string& path, const stackcore::FrameBuffer& image,
                        OutputFormat format, const stackcore::ImageMetadata& metadata) {
    if (format == OutputFormat::FitsFloat32) {
        stackcore::write_fits_float32(path, image, metadata);
        return;
    }
    if (format == OutputFormat::Png16) {
        stackcore::write_png16(path, image, metadata);
        return;
    }
    stackcore::write_tiff(path, image,
                          format == OutputFormat::TiffFloat32 ? stackcore::TiffFormat::Float32
                                                              : stackcore::TiffFormat::UInt16,
                          metadata);
}

NSArray* LSInputFileTypes() {
    return @[
        @"ser", @"avi", @"mov", @"mp4", @"m4v", @"tif", @"tiff", @"png", @"fit", @"fits", @"fts", @"jpg", @"jpeg",
        // カメラのRAW（stackcore::is_raw_image_path と同じ並び）
        @"cr2", @"dng", @"cr3", @"crw", @"nef", @"nrw", @"arw", @"srf", @"sr2", @"raf", @"orf",
        @"rw2", @"raw", @"rwl", @"pef", @"srw", @"3fr", @"fff", @"iiq", @"erf", @"kdc", @"dcr",
        @"mrw", @"mos", @"mef", @"gpr"
    ];
}

NSTextField* MakeLabel(NSString* text) {
    NSTextField* label = [[[NSTextField alloc] init] autorelease];
    [label setStringValue:text];
    [label setBezeled:NO];
    [label setDrawsBackground:NO];
    [label setEditable:NO];
    [label setSelectable:NO];
    [label setFont:[NSFont systemFontOfSize:11.0]];
    [label setTranslatesAutoresizingMaskIntoConstraints:NO];
    return label;
}

JobResult run_job(const JobRequest& req, const stackcore::ProgressFn& progress) {
    JobResult out;
    out.stage = req.stage;
    try {
        std::unique_ptr<stackcore::VideoSource> source =
            stackcore::open_video(req.path, req.options);
        if (req.low_memory) source->set_low_memory(true);
        stackcore::MapStackSettings job_settings = req.settings;
        job_settings.low_memory = req.low_memory;

        if (req.stage == JobStage::Quality) {
            out.quality = std::make_shared<stackcore::GlobalStageReport>(
                stackcore::evaluate_frame_quality(*source, job_settings.global,
                                                  job_settings.raw_cfa, progress));
            out.frames = out.quality->frames;
            return out;
        }

        if (req.stage == JobStage::Alignment) {
            if (!req.quality) {
                throw std::runtime_error("アライメント: 先に品質評価を実行してください");
            }
            out.global = std::make_shared<stackcore::GlobalStageReport>(
                stackcore::run_global_alignment(*source, job_settings.global,
                                                job_settings.raw_cfa, *req.quality, progress));
            out.frames = out.global->frames;
            if (!req.global_only) {
                auto report = std::make_shared<stackcore::MapStackReport>();
                out.analysis = std::make_shared<stackcore::AnalysisData>(
                    stackcore::analyze_map_alignment(*source, job_settings, *out.global,
                                                     progress, *report));
                report->global = *out.global;
                out.map_report = report;
                out.frames = out.analysis->frames;
            }
            return out;
        }

        if (req.stage == JobStage::Stack) {
            if (req.global_only) {
                if (!req.global) {
                    throw std::runtime_error("スタック: 先にアライメントを実行してください");
                }
                const std::vector<stackcore::FrameInfo> selected =
                    stackcore::select_top_frames(req.global->frames,
                                                 job_settings.reference_top_percent);
                out.frames = req.global->frames;
                out.image = std::make_shared<stackcore::FrameBuffer>(
                    stackcore::build_global_reference(*source, *req.global, selected,
                                                      job_settings.raw_cfa, progress,
                                                      job_settings.normalize_brightness));
                for (const stackcore::FrameInfo& f : selected) out.stacked_frames.push_back(f.index);
                out.frames_combined = static_cast<int>(selected.size());
            } else {
                if (!req.analysis) {
                    throw std::runtime_error("スタック: 先にアライメントを実行してください");
                }
                stackcore::MapStackReport report;
                out.analysis = req.analysis;
                out.frames = req.analysis->frames;
                out.image = std::make_shared<stackcore::FrameBuffer>(
                    stackcore::stack_from_analysis(*source, job_settings, *req.analysis,
                                                   progress, report));
                out.stacked_frames = req.analysis->analyzed_indices;
                out.frames_combined = report.frames_per_ap;
            }
            return out;
        }

        // GUI自己検証用の一括経路。通常のGUIボタンはここを通らない。
        if (req.global_only) {
            out.global = std::make_shared<stackcore::GlobalStageReport>(
                stackcore::run_global_stage(*source, job_settings.global,
                                            job_settings.raw_cfa, progress));
            out.frames = out.global->frames;
            const std::vector<stackcore::FrameInfo> selected = stackcore::select_top_frames(
                out.global->frames, job_settings.reference_top_percent);
            out.image = std::make_shared<stackcore::FrameBuffer>(
                stackcore::build_global_reference(*source, *out.global, selected,
                                                  job_settings.raw_cfa, progress,
                                                  job_settings.normalize_brightness));
            for (const stackcore::FrameInfo& f : selected) out.stacked_frames.push_back(f.index);
            out.frames_combined = static_cast<int>(selected.size());
        } else {
            auto report = std::make_shared<stackcore::MapStackReport>();
            out.analysis = std::make_shared<stackcore::AnalysisData>(
                stackcore::analyze_map_stack(*source, job_settings, progress, *report));
            out.global =
                std::make_shared<stackcore::GlobalStageReport>(report->global);
            out.map_report = report;
            out.frames = out.analysis->frames;
            stackcore::MapStackReport stack_report;
            out.image = std::make_shared<stackcore::FrameBuffer>(stackcore::stack_from_analysis(
                *source, job_settings, *out.analysis, progress, stack_report));
            out.stacked_frames = out.analysis->analyzed_indices;
            out.frames_combined = stack_report.frames_per_ap;
        }
    } catch (const stackcore::Cancelled&) {
        out.cancelled = true;
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

// AP別の平均品質（ヒートマップ用）。行列の並びは points 順 → analyzed_indices 順。
std::vector<double> ap_mean_quality(const stackcore::AnalysisData& analysis) {
    std::vector<double> out;
    const std::size_t ap_count = analysis.points.size();
    const std::size_t frames = analysis.analyzed_indices.size();
    if (ap_count == 0 || frames == 0 || analysis.matrix.size() < ap_count * frames) return out;

    out.resize(ap_count, 0.0);
    for (std::size_t a = 0; a < ap_count; ++a) {
        double sum = 0.0;
        for (std::size_t f = 0; f < frames; ++f) {
            sum += analysis.matrix[a * frames + f].quality;
        }
        out[a] = sum / static_cast<double>(frames);
    }
    return out;
}

