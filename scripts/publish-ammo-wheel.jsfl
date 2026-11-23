var as3Path = "__AS3_PATH__";
var sourceFla = "__SOURCE_FLA_URI__";
var ammoIconNames = __AMMO_ICON_NAMES__;
var outputFla = "__FLA_URI__";
var logURI = "__LOG_URI__";
var outputLog = "__OUTPUT_URI__";
var doc = null;

function finish(status, message) {
    FLfile.write(logURI, status + "\n" + message, "");
}

try {
    doc = fl.openDocument(sourceFla);
    if (!doc) throw new Error("Animate could not open the integrated ammo wheel FLA");
    doc.docClass = "MWBAmmoSwitchWheel";
    doc.sourcePath = as3Path;
    doc.as3PackagePaths = ".";
    var linkedBlade = false;
    var linkedIcons = {};
    for (var i = 0; i < doc.library.items.length; i++) {
        var linkedItem = doc.library.items[i];
        if (linkedItem.linkageExportForAS && linkedItem.linkageClassName == "FavoritesWheelCategoryBlade")
            linkedBlade = true;
        if (linkedItem.linkageExportForAS && typeof linkedItem.linkageClassName == "string" && linkedItem.linkageClassName.indexOf("AmmoIcon_") == 0)
            linkedIcons[linkedItem.linkageClassName.substring("AmmoIcon_".length)] = true;
    }
    if (!linkedBlade) throw new Error("Integrated source FLA is missing FavoritesWheelCategoryBlade");
    for (var iconIndex = 0; iconIndex < ammoIconNames.length; iconIndex++) {
        if (!linkedIcons[ammoIconNames[iconIndex]])
            throw new Error("Integrated source FLA is missing linked ammo icon: " + ammoIconNames[iconIndex]);
    }
    if (!fl.saveDocument(doc, outputFla)) throw new Error("Animate could not save the temporary wheel document");

    fl.outputPanel.clear();
    doc.publish();
    fl.outputPanel.save(outputLog);
    doc.close(false);
    doc = null;
    finish("OK", "Published the integrated MWB ammo switch wheel with " + ammoIconNames.length + " linked ammo icons");
} catch (error) {
    if (doc) {
        try { doc.close(false); } catch (closeError) {}
        doc = null;
    }
    FLfile.write(outputLog, String(error), "");
    finish("ERROR", String(error));
}
