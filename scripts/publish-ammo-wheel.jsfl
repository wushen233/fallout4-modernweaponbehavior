var as3Path = "__AS3_PATH__";
var outputFla = "__FLA_URI__";
var logURI = "__LOG_URI__";
var outputLog = "__OUTPUT_URI__";
var doc = null;

function finish(status, message) {
    FLfile.write(logURI, status + "\n" + message, "");
}

try {
    doc = fl.createDocument("timeline");
    if (!doc) throw new Error("Animate could not create a new timeline document");
    doc.width = 1920;
    doc.height = 1080;
    doc.frameRate = 30;
    doc.docClass = "MWBAmmoSwitchWheel";
    doc.sourcePath = as3Path;
    doc.as3PackagePaths = ".";
    if (!fl.saveDocument(doc, outputFla)) throw new Error("Animate could not save the temporary wheel document");

    fl.outputPanel.clear();
    doc.publish();
    fl.outputPanel.save(outputLog);
    doc.close(false);
    doc = null;
    finish("OK", "Created and published the MWB ammo switch wheel");
} catch (error) {
    if (doc) {
        try { doc.close(false); } catch (closeError) {}
        doc = null;
    }
    FLfile.write(outputLog, String(error), "");
    finish("ERROR", String(error));
}
