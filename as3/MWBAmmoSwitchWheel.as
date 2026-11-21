package
{
    import flash.display.MovieClip;
    import flash.display.Shape;
    import flash.display.Sprite;
    import flash.events.Event;
    import flash.events.MouseEvent;
    import flash.text.TextField;
    import flash.text.TextFieldAutoSize;
    import flash.text.TextFormat;

    public class MWBAmmoSwitchWheel extends MovieClip
    {
        private static const INNER_RADIUS:Number = 82;
        private static const OUTER_RADIUS:Number = 248;
        private static const DEAD_ZONE:Number = 34;
        private static const MAX_LABELS:int = 12;

        private var backdrop:Shape;
        private var sectors:Sprite;
        private var labels:Sprite;
        private var center:Sprite;
        private var title:TextField;
        private var quantity:TextField;
        private var names:Array = [];
        private var counts:Array = [];
        private var selectedIndex:int = -1;
        private var currentIndex:int = -1;
        private var centerX:Number;
        private var centerY:Number;

        public function MWBAmmoSwitchWheel()
        {
            stop();
            addEventListener(Event.ADDED_TO_STAGE, onAddedToStage, false, 0, true);
        }

        public function SetOptions(a_names:Array, a_counts:Array, a_currentIndex:int):void
        {
            names = a_names ? a_names.concat() : [];
            counts = a_counts ? a_counts.concat() : [];
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

        private function onAddedToStage(a_event:Event):void
        {
            removeEventListener(Event.ADDED_TO_STAGE, onAddedToStage);
            centerX = stage.stageWidth * 0.5;
            centerY = stage.stageHeight * 0.5;

            backdrop = new Shape();
            backdrop.graphics.beginFill(0x05090C, 0.24);
            backdrop.graphics.drawRect(0, 0, stage.stageWidth, stage.stageHeight);
            backdrop.graphics.endFill();
            addChild(backdrop);

            sectors = new Sprite();
            labels = new Sprite();
            center = new Sprite();
            sectors.mouseEnabled = false;
            labels.mouseEnabled = false;
            center.mouseEnabled = false;
            addChild(sectors);
            addChild(labels);
            addChild(center);

            stage.addEventListener(MouseEvent.MOUSE_MOVE, onMouseMove, false, 0, true);
            stage.addEventListener(Event.RESIZE, onResize, false, 0, true);
            render();
        }

        private function onResize(a_event:Event):void
        {
            centerX = stage.stageWidth * 0.5;
            centerY = stage.stageHeight * 0.5;
            backdrop.graphics.clear();
            backdrop.graphics.beginFill(0x05090C, 0.24);
            backdrop.graphics.drawRect(0, 0, stage.stageWidth, stage.stageHeight);
            backdrop.graphics.endFill();
            render();
        }

        private function onMouseMove(a_event:MouseEvent):void
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

            sectors.graphics.clear();
            labels.removeChildren();
            center.graphics.clear();

            const count:int = names.length;
            if (count <= 0)
                return;

            const step:Number = Math.PI * 2 / count;
            for (var i:int = 0; i < count; i++)
            {
                const start:Number = -Math.PI * 0.5 + step * i;
                const end:Number = start + step;
                const active:Boolean = i == selectedIndex;
                const equipped:Boolean = i == currentIndex;
                const fill:uint = active ? 0x507E5A : 0x20292F;
                const alpha:Number = active ? 0.97 : 0.90;
                sectors.graphics.lineStyle(equipped ? 2.5 : 1.2, equipped ? 0xE7C66A : 0x93A0A6, equipped ? 0.95 : 0.58, true);
                sectors.graphics.beginFill(fill, alpha);
                drawWedge(sectors.graphics, centerX, centerY, INNER_RADIUS, OUTER_RADIUS, start, end);
                sectors.graphics.endFill();

                const middle:Number = (start + end) * 0.5;
                const radius:Number = (INNER_RADIUS + OUTER_RADIUS) * 0.5;
                const label:TextField = makeText();
                label.width = Math.min(160, Math.max(62, step * radius * 1.42));
                label.height = count <= MAX_LABELS ? 40 : 24;
                label.x = centerX + Math.cos(middle) * radius - label.width * 0.5;
                label.y = centerY + Math.sin(middle) * radius - label.height * 0.5;
                label.defaultTextFormat = new TextFormat("$MAIN_Font", count <= MAX_LABELS ? 16 : 14, 0xF4F4F0, false, false, false, null, null, "center");
                label.text = count <= MAX_LABELS ? String(names[i]) : String(i + 1);
                label.setTextFormat(label.defaultTextFormat);
                label.mouseEnabled = false;
                labels.addChild(label);
            }

            center.graphics.lineStyle(2, 0xE7C66A, 0.8, true);
            center.graphics.beginFill(0x11181D, 0.97);
            center.graphics.drawCircle(centerX, centerY, INNER_RADIUS - 3);
            center.graphics.endFill();
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

            title = makeText();
            title.width = 142;
            title.height = 48;
            title.x = centerX - title.width * 0.5;
            title.y = centerY - 32;
            title.defaultTextFormat = new TextFormat("$MAIN_Font", 17, 0xFFFFFF, true, false, false, null, null, "center");
            title.text = String(names[selectedIndex]);
            title.setTextFormat(title.defaultTextFormat);
            title.mouseEnabled = false;
            center.addChild(title);

            quantity = makeText();
            quantity.width = 120;
            quantity.height = 28;
            quantity.x = centerX - quantity.width * 0.5;
            quantity.y = centerY + 15;
            const equipped:Boolean = selectedIndex == currentIndex;
            quantity.defaultTextFormat = new TextFormat("$MAIN_Font", 15, equipped ? 0xE7C66A : 0xC9D1D4, false, false, false, null, null, "center");
            quantity.text = String(counts[selectedIndex]);
            quantity.setTextFormat(quantity.defaultTextFormat);
            quantity.mouseEnabled = false;
            center.addChild(quantity);
        }

        private function makeText():TextField
        {
            const field:TextField = new TextField();
            field.selectable = false;
            field.multiline = true;
            field.wordWrap = true;
            field.autoSize = TextFieldAutoSize.NONE;
            return field;
        }

        private function drawWedge(a_graphics:Object, a_cx:Number, a_cy:Number, a_inner:Number, a_outer:Number, a_start:Number, a_end:Number):void
        {
            const steps:int = Math.max(3, Math.ceil((a_end - a_start) * 36));
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
