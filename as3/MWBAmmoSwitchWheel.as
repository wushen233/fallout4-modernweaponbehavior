package
{
    import flash.display.MovieClip;
    import flash.display.DisplayObject;
    import flash.display.Shape;
    import flash.display.Sprite;
    import flash.events.Event;
    import flash.events.MouseEvent;
    import flash.filters.DropShadowFilter;
    import flash.text.AntiAliasType;
    import flash.text.TextField;
    import flash.text.TextFieldAutoSize;
    import flash.text.TextFormat;
    import flash.text.TextFormatAlign;
import Shared.AS3.BSButtonHintData;

    public class MWBAmmoSwitchWheel extends MovieClip
    {
        // Static references keep Animate's linked SVG symbols in the published
        // SWF even though their instances are resolved by name at runtime.
        private static const EMBEDDED_AMMO_ICON_CLASSES:Array = [
            AmmoIcon_ballistic, AmmoIcon_armor_piercing, AmmoIcon_hollow_point,
            AmmoIcon_subsonic, AmmoIcon_incendiary, AmmoIcon_explosive,
            AmmoIcon_shotgun, AmmoIcon_energy_ammo, AmmoIcon_fusion_cell,
            AmmoIcon_plasma, AmmoIcon_cryo, AmmoIcon_missile
        ];

        // FO76's radial wheel uses a narrower annulus and a larger center disk.
        private static const INNER_RADIUS:Number = 150;
        private static const OUTER_RADIUS:Number = 246;
        private static const DEAD_ZONE:Number = 34;
        private static const MAX_SECTOR_NAMES:int = 8;
        private static const ARC_SEGMENTS_PER_RADIAN:Number = 180;
        private static const GOLD:uint = 0xE9D17D;
        private static const CREAM:uint = 0xFFF0C2;

        private var sectors:Sprite;
        private var labels:Sprite;
        private var slotNumbers:Sprite;
        private var center:Sprite;
        private var categoryTab:Sprite;
        private var categoryTabText:TextField;
        private var hintSource:AmmoWheelHintSource;
        private var title:TextField;
        private var quantity:TextField;
        private var damage:TextField;
        private var centerAmmoIcon:DisplayObject;
        private var names:Array = [];
        private var counts:Array = [];
        private var icons:Array = [];
        private var damages:Array = [];
        private var selectedIndex:int = -1;
        private var currentIndex:int = -1;
        private var centerX:Number;
        private var centerY:Number;

        public function MWBAmmoSwitchWheel()
        {
            stop();
            addEventListener(Event.ADDED_TO_STAGE, onAddedToStage, false, 0, true);
        }

        public function SetOptions(a_names:Array, a_counts:Array, a_icons:Array, a_damages:Array, a_currentIndex:int):void
        {
            names = a_names ? a_names.concat() : [];
            counts = a_counts ? a_counts.concat() : [];
            icons = a_icons ? a_icons.concat() : [];
            damages = a_damages ? a_damages.concat() : [];
            currentIndex = a_currentIndex;
            selectedIndex = currentIndex >= 0 && currentIndex < names.length ? currentIndex : 0;
            render();
        }

        public function SetStickDirection(a_x:Number, a_y:Number):void
        {
            if (a_x * a_x + a_y * a_y < 0.16)
                return;
            selectDirection(a_x, -a_y);
        }

        public function GetSelectedIndex():int
        {
            return selectedIndex;
        }

        public function DebugInfo():String
        {
            return "options=" + names.length + ", counts=" + counts.length + ", selected=" + selectedIndex +
                ", stage=" + (stage ? stage.stageWidth + "x" + stage.stageHeight : "none") +
                ", rootAlpha=" + alpha + ", wheelLayers=" + (numChildren > 0 ? numChildren - 1 : 0) +
                ", sectors=" + (sectors ? sectors.width + "x" + sectors.height : "none") +
                ", labels=" + (labels ? labels.numChildren : -1) + ", centerChildren=" + (center ? center.numChildren : -1) +
                ", selectedTitle=" + Boolean(title && title.parent) + ", centerReady=" + Boolean(center);
        }

        private function onAddedToStage(a_event:Event):void
        {
            removeEventListener(Event.ADDED_TO_STAGE, onAddedToStage);
            centerX = stage.stageWidth * 0.5;
            centerY = stage.stageHeight * 0.5;

            sectors = new Sprite();
            labels = new Sprite();
            slotNumbers = new Sprite();
            center = new Sprite();
            sectors.mouseEnabled = false;
            labels.mouseEnabled = false;
            slotNumbers.mouseEnabled = false;
            center.mouseEnabled = false;
            addChild(sectors);
            addChild(labels);
            addChild(slotNumbers);
            addChild(center);
            setupCategoryTab();
            setupGlobalHintSource();

            stage.addEventListener(MouseEvent.MOUSE_MOVE, handleStageMouseMove, false, 0, true);
            stage.addEventListener(Event.RESIZE, onResize, false, 0, true);
            render();
        }

        private function onResize(a_event:Event):void
        {
            centerX = stage.stageWidth * 0.5;
            centerY = stage.stageHeight * 0.5;
            positionCategoryTab();
            render();
        }

        private function handleStageMouseMove(a_event:MouseEvent):void
        {
            const dx:Number = stage.mouseX - centerX;
            const dy:Number = stage.mouseY - centerY;
            if (dx * dx + dy * dy <= DEAD_ZONE * DEAD_ZONE)
                return;
            selectDirection(dx, dy);
        }

        private function selectDirection(a_x:Number, a_y:Number):void
        {
            if (names.length == 0)
                return;

            var angle:Number = Math.atan2(a_y, a_x) + Math.PI * 0.5;
            angle = (angle + Math.PI * 2) % (Math.PI * 2);
            selectedIndex = Math.floor((angle / (Math.PI * 2)) * names.length) % names.length;
            render();
        }

        private function render():void
        {
            if (!sectors || !labels || !center)
                return;

            while (sectors.numChildren > 0)
                sectors.removeChildAt(sectors.numChildren - 1);
            while (labels.numChildren > 0)
                labels.removeChildAt(labels.numChildren - 1);
            while (slotNumbers.numChildren > 0)
                slotNumbers.removeChildAt(slotNumbers.numChildren - 1);
            center.graphics.clear();

            const count:int = names.length;
            if (count <= 0)
                return;

            const step:Number = Math.PI * 2 / count;
            const outerDisk:Shape = new Shape();
            outerDisk.graphics.lineStyle(3, 0x07090B, 1, true);
            outerDisk.graphics.beginFill(0x17191B, 0.78);
            outerDisk.graphics.drawCircle(centerX, centerY, OUTER_RADIUS + 3);
            outerDisk.graphics.endFill();
            sectors.addChild(outerDisk);

            for (var i:int = 0; i < count; i++)
            {
                const gap:Number = Math.min(0.016, step * 0.03);
                const start:Number = -Math.PI * 0.5 + step * i + gap;
                const end:Number = -Math.PI * 0.5 + step * (i + 1) - gap;
                const active:Boolean = i == selectedIndex;
                const equipped:Boolean = i == currentIndex;
                // Keep each slot's dark channel and inset face in separate
                // Graphics contexts. This matches FO76's layered slot edges
                // and avoids GFx joining paths from neighboring sectors.
                const basePanel:Shape = new Shape();
                basePanel.graphics.beginFill(0x111315, 0.48);
                drawWedge(basePanel.graphics, centerX, centerY, INNER_RADIUS, OUTER_RADIUS, start, end);
                basePanel.graphics.endFill();
                sectors.addChild(basePanel);

                const panel:Shape = new Shape();
                const panelFill:uint = active ? 0x39362D : 0x24272A;
                const panelLine:uint = active ? GOLD : 0x15181A;
                const panelInset:Number = active ? gap * 0.2 : Math.min(0.012, step * 0.022);
                panel.graphics.lineStyle(active ? 2.5 : 1.6, panelLine, active ? 1 : 0.35, true);
                panel.graphics.beginFill(panelFill, active ? 0.62 : 0.42);
                drawWedge(panel.graphics, centerX, centerY,
                    active ? INNER_RADIUS + 1 : INNER_RADIUS + 6,
                    active ? OUTER_RADIUS - 1 : OUTER_RADIUS - 7,
                    start + panelInset, end - panelInset);
                panel.graphics.endFill();
                sectors.addChild(panel);

                // Match the 42px favorite-wheel slots. The renderer fits the
                // visible artwork bounds, so SVG viewBox whitespace cannot
                // shift or shrink the glyph.
                const ammoMark:DisplayObject = AmmoIconRenderer.create(
                    iconAt(i), 42, active ? CREAM : 0xC5B985);
                const markAngle:Number = (start + end) * 0.5;
                // Place the icon's visual center at the annular-sector area
                // centroid rather than biasing it toward the outer rim.
                const markRadius:Number = (2 / 3) *
                    (OUTER_RADIUS * OUTER_RADIUS * OUTER_RADIUS - INNER_RADIUS * INNER_RADIUS * INNER_RADIUS) /
                    (OUTER_RADIUS * OUTER_RADIUS - INNER_RADIUS * INNER_RADIUS);
                ammoMark.x = centerX + Math.cos(markAngle) * markRadius;
                ammoMark.y = centerY + Math.sin(markAngle) * markRadius;
                ammoMark.alpha = active ? 1 : 0.9;
                sectors.addChild(ammoMark);

                if (equipped)
                {
                    const markerAngle:Number = (start + end) * 0.5;
                    const marker:Shape = new Shape();
                    marker.graphics.beginFill(CREAM, 0.95);
                    marker.graphics.drawCircle(centerX + Math.cos(markerAngle) * (INNER_RADIUS + 17), centerY + Math.sin(markerAngle) * (INNER_RADIUS + 17), 3.5);
                    marker.graphics.endFill();
                    sectors.addChild(marker);
                }

            }

            const innerRing:Shape = new Shape();
            innerRing.graphics.lineStyle(2.5, 0x343A40, 0.95, true);
            innerRing.graphics.drawCircle(centerX, centerY, INNER_RADIUS - 1);
            sectors.addChild(innerRing);
            if (centerAmmoIcon && centerAmmoIcon.parent == center)
                center.removeChild(centerAmmoIcon);
            centerAmmoIcon = null;
            center.graphics.lineStyle(2, 0x373D42, 1, true);
            center.graphics.beginFill(0x17191B, 0.78);
            center.graphics.drawCircle(centerX, centerY, INNER_RADIUS - 3);
            center.graphics.endFill();
            if (selectedIndex >= 0 && selectedIndex < names.length)
            {
                centerAmmoIcon = AmmoIconRenderer.create(iconAt(selectedIndex), 54, CREAM);
                centerAmmoIcon.x = centerX;
                centerAmmoIcon.y = centerY - 42;
                center.addChild(centerAmmoIcon);
            }
            renderCenter();
        }

        private function renderCenter():void
        {
            if (selectedIndex < 0 || selectedIndex >= names.length)
                return;

            if (title && title.parent)
                title.parent.removeChild(title);
            if (quantity && quantity.parent)
                quantity.parent.removeChild(quantity);
            if (damage && damage.parent)
                damage.parent.removeChild(damage);

            title = makeText();
            title.width = 720;
            title.height = 52;
            title.x = centerX - title.width * 0.5;
            title.y = centerY + OUTER_RADIUS + 38;
            const name:String = String(names[selectedIndex]);
            title.defaultTextFormat = centeredFormat(36, GOLD, false);
            title.embedFonts = true;
            title.text = name;
            title.setTextFormat(title.defaultTextFormat);
            title.filters = [new DropShadowFilter(1, 45, 0x000000, 0.9, 2, 2, 2, 1)];
            title.mouseEnabled = false;
            center.addChild(title);

            quantity = makeText();
            quantity.width = 200;
            quantity.height = 24;
            quantity.x = centerX - quantity.width * 0.5;
            // Favorites wheel centers the title/count block and leaves two
            // points between its measured title line and the count line.
            quantity.y = title.y + title.textHeight + 6;
            quantity.defaultTextFormat = centeredFormat(16, CREAM, false);
            quantity.embedFonts = true;
            quantity.text = "(" + String(counts[selectedIndex]) + ")";
            quantity.setTextFormat(quantity.defaultTextFormat);
            quantity.filters = [new DropShadowFilter(1, 45, 0x000000, 0.9, 2, 2, 2, 1)];
            quantity.mouseEnabled = false;
            center.addChild(quantity);

            damage = makeText();
            damage.width = 260;
            damage.height = 138;
            damage.x = centerX - damage.width * 0.5;
            damage.y = centerY + 10;
            damage.defaultTextFormat = centeredFormat(18, CREAM, true);
            damage.text = translate("$MWB_AmmoWheel_Damage") + ": " + damageAt(selectedIndex);
            damage.setTextFormat(damage.defaultTextFormat);
            damage.filters = [new DropShadowFilter(1, 45, 0x000000, 0.9, 2, 2, 2, 1)];
            damage.mouseEnabled = false;
            center.addChild(damage);
        }

        private function damageAt(a_index:int):String
        {
            if (a_index < 0 || a_index >= damages.length || damages[a_index] == null)
                return "0";

            const lines:Array = String(damages[a_index]).split("\n");
            for (var i:int = 1; i < lines.length; i++)
            {
                const separator:int = String(lines[i]).indexOf(":");
                if (separator > 0)
                    lines[i] = translate(String(lines[i]).substring(0, separator)) + String(lines[i]).substring(separator);
            }
            return lines.join("\n");
        }

        private function iconAt(a_index:int):String
        {
            return a_index >= 0 && a_index < icons.length && icons[a_index] != null && String(icons[a_index]) != "" ? String(icons[a_index]) : "ballistic";
        }

        private function setupCategoryTab():void
        {
            categoryTab = new Sprite();
            categoryTab.mouseEnabled = categoryTab.mouseChildren = false;
            categoryTab.scaleX = categoryTab.scaleY = 1.0;

            // This linked vector is the BladeFill_mc shape exported from the
            // FO76 favorites wheel and embedded by the SWF publisher.
            const blade:MovieClip = new FavoritesWheelCategoryBlade();
            blade.mouseEnabled = blade.mouseChildren = false;
            blade.scaleX = 0.82;
            categoryTab.addChild(blade);

            categoryTabText = makeText();
            categoryTabText.width = 160;
            categoryTabText.height = 36;
            categoryTabText.x = 24;
            categoryTabText.y = 2;
            const tabFormat:TextFormat = new TextFormat("$MAIN_Font_Bold", 26, 0x171A1C, true);
            tabFormat.align = TextFormatAlign.LEFT;
            categoryTabText.defaultTextFormat = tabFormat;
            categoryTabText.text = translate("$MWB_AmmoWheel_Category");
            categoryTabText.setTextFormat(categoryTabText.defaultTextFormat);
            categoryTab.addChild(categoryTabText);
            addChild(categoryTab);
            positionCategoryTab();
        }

        private function setupGlobalHintSource():void
        {
            hintSource = new AmmoWheelHintSource();
            hintSource.name = "AmmoWheelHintSource";
            hintSource.visible = false;
            addChild(hintSource);
            const hints:Vector.<BSButtonHintData> = new Vector.<BSButtonHintData>();
            const hint:BSButtonHintData = new BSButtonHintData("$MWB_AmmoWheel_SelectHint", "", "", "", 1, null);
            hint.ButtonEnabled = false;
            hints.push(hint);
            if (hintSource.SetButtonHintData != null)
                hintSource.SetButtonHintData(hints);
            else
                addEventListener(Event.ENTER_FRAME, submitGlobalHints, false, 0, true);
        }

        private function submitGlobalHints(a_event:Event):void
        {
            if (hintSource == null || hintSource.SetButtonHintData == null)
                return;
            removeEventListener(Event.ENTER_FRAME, submitGlobalHints);
            const hints:Vector.<BSButtonHintData> = new Vector.<BSButtonHintData>();
            const hint:BSButtonHintData = new BSButtonHintData("$MWB_AmmoWheel_SelectHint", "", "", "", 1, null);
            hint.ButtonEnabled = false;
            hints.push(hint);
            hintSource.SetButtonHintData(hints);
        }

        private function positionCategoryTab():void
        {
            if (!categoryTab || !stage)
                return;
            categoryTab.x = 0;
            categoryTab.y = stage.stageHeight - 63;
        }

        private function translate(a_key:String):String
        {
            const field:TextField = new TextField();
            field.text = a_key;
            return field.text;
        }

        private function centeredFormat(a_size:int, a_color:uint, a_bold:Boolean):TextFormat
        {
            const format:TextFormat = new TextFormat("$MAIN_Font", a_size, a_color, a_bold);
            format.align = TextFormatAlign.CENTER;
            return format;
        }

        private function makeText():TextField
        {
            const field:TextField = new TextField();
            field.selectable = false;
            field.multiline = true;
            field.wordWrap = true;
            field.autoSize = TextFieldAutoSize.NONE;
            field.antiAliasType = AntiAliasType.ADVANCED;
            return field;
        }

        private function drawWedge(a_graphics:Object, a_cx:Number, a_cy:Number, a_inner:Number, a_outer:Number, a_start:Number, a_end:Number):void
        {
            // GFx draws these arcs as straight line segments. Keep the chord
            // length small enough that the narrow left-hand sectors stay smooth.
            const steps:int = Math.max(3, Math.ceil((a_end - a_start) * ARC_SEGMENTS_PER_RADIAN));
            a_graphics.moveTo(a_cx + Math.cos(a_start) * a_outer, a_cy + Math.sin(a_start) * a_outer);
            for (var i:int = 1; i <= steps; i++)
            {
                const angle:Number = a_start + (a_end - a_start) * i / steps;
                a_graphics.lineTo(a_cx + Math.cos(angle) * a_outer, a_cy + Math.sin(angle) * a_outer);
            }
            for (var j:int = steps; j >= 0; j--)
            {
                const angleInner:Number = a_start + (a_end - a_start) * j / steps;
                a_graphics.lineTo(a_cx + Math.cos(angleInner) * a_inner, a_cy + Math.sin(angleInner) * a_inner);
            }
            a_graphics.lineTo(a_cx + Math.cos(a_start) * a_outer, a_cy + Math.sin(a_start) * a_outer);
        }
    }
}
