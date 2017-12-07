//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import Foundation
@_exported import AppKit

extension NSCursor : _DefaultCustomPlaygroundQuickLookable {
  public var _defaultCustomPlaygroundQuickLook: PlaygroundQuickLook {
    return .image(image)
  }
}

internal struct _NSViewQuickLookState {
  static var views = Set<NSView>()
}

extension NSView : _DefaultCustomPlaygroundQuickLookable {
  public var _defaultCustomPlaygroundQuickLook: PlaygroundQuickLook {
    // if you set NSView.needsDisplay, you can get yourself in a recursive scenario where the same view
    // could need to draw itself in order to get a QLObject for itself, which in turn if your code was
    // instrumented to log on-draw, would cause yourself to get back here and so on and so forth
    // until you run out of stack and crash
    // This code checks that we aren't trying to log the same view recursively - and if so just returns
    // an empty view, which is probably a safer option than crashing
    // FIXME: is there a way to say "cacheDisplayInRect butDoNotRedrawEvenIfISaidSo"?
    if _NSViewQuickLookState.views.contains(self) {
      return .view(NSImage())
    } else {
      _NSViewQuickLookState.views.insert(self)
      let result: PlaygroundQuickLook
      if let b = bitmapImageRepForCachingDisplay(in: bounds) {
        cacheDisplay(in: bounds, to: b)
        result = .view(b)
      } else {
        result = .view(NSImage())
      }
      _NSViewQuickLookState.views.remove(self)
      return result
    }
  }
}

// Overlays for variadics.

public extension NSGradient {
  convenience init?(colorsAndLocations objects: (NSColor, CGFloat)...) {
    self.init(
      colors: objects.map { $0.0 },
      atLocations: objects.map { $0.1 },
      colorSpace: NSColorSpace.genericRGB)
  }
}

// Fix the ARGV type of NSApplicationMain, which nonsensically takes
// argv as a const char**.
public func NSApplicationMain(
  _ argc: Int32, _ argv: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>
) -> Int32 {
  return argv.withMemoryRebound(to: UnsafePointer<CChar>.self, capacity: Int(argc)) {
    __NSApplicationMain(argc, $0)
  }
}

extension NSApplication {
  @available(swift 4)
  public static func loadApplication() {
    NSApplicationLoad()
  }
}

extension NSColor : _ExpressibleByColorLiteral {
  @nonobjc
  public required convenience init(_colorLiteralRed red: Float, green: Float,
                                   blue: Float, alpha: Float) {
    self.init(red: CGFloat(red), green: CGFloat(green),
              blue: CGFloat(blue), alpha: CGFloat(alpha))
  }
}

public typealias _ColorLiteralType = NSColor

extension NSImage : _ExpressibleByImageLiteral {
  private convenience init!(failableImageLiteral name: String) {
    self.init(named: name)
  }

  @nonobjc
  public required convenience init(imageLiteralResourceName name: String) {
    self.init(failableImageLiteral: name)
  }
}

public typealias _ImageLiteralType = NSImage
