//XFAIL: *

//RUN: %target-prepare-obfuscation-with-storyboard "UnsupportedLayout" %target-run-full-obfuscation

import AppKit

class ViewController {
  
  @IBOutlet weak var informativeLabel: UIButton!

  @IBAction func switchInputRepresentation(_ sender: UIButton) {
  }

}
