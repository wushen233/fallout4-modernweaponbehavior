package
{
    import flash.display.DisplayObject;
    import flash.display.Graphics;
    import flash.display.Shape;
    import flash.display.Sprite;
    import flash.filters.DropShadowFilter;
    import flash.geom.ColorTransform;
    import flash.utils.getDefinitionByName;

    /** Shared vector glyphs for ammo icons in the wheel and future HUD views. */
    public final class AmmoIconRenderer
    {
        public static function create(a_icon:String, a_targetSize:Number, a_fill:uint):DisplayObject
        {
            if (a_targetSize <= 0)
                return null;

            const symbol:String = symbolFor(a_icon);
            if (symbol != "")
            {
                try
                {
                    const symbolClass:Class = getDefinitionByName("AmmoIcon_" + symbol) as Class;
                    if (symbolClass != null)
                    {
                        const asset:DisplayObject = new symbolClass() as DisplayObject;
                        if (asset != null)
                        {
                            const container:Sprite = new Sprite();
                            container.addChild(asset);
                            const sourceBounds:Object = asset.getBounds(container);
                            const sourceSize:Number = Math.max(sourceBounds.width, sourceBounds.height);
                            if (sourceSize <= 0)
                                return null;

                            const scale:Number = a_targetSize / sourceSize;
                            asset.scaleX = scale;
                            asset.scaleY = scale;
                            const visibleBounds:Object = asset.getBounds(container);
                            asset.x -= (visibleBounds.left + visibleBounds.right) * 0.5;
                            asset.y -= (visibleBounds.top + visibleBounds.bottom) * 0.5;
                            const color:ColorTransform = new ColorTransform(
                                0, 0, 0, 1,
                                (a_fill >> 16) & 0xFF,
                                (a_fill >> 8) & 0xFF,
                                a_fill & 0xFF,
                                0);
                            asset.transform.colorTransform = color;
                            container.filters = [new DropShadowFilter(1, 45, 0x000000, 0.6, 2, 2, 1, 1, false, false, false)];
                            return container;
                        }
                    }
                }
                catch (a_error:Error)
                {
                    // Keep the procedural glyph as a safe fallback if a SWF
                    // was published without the embedded symbol library.
                }
            }

            const fallback:Shape = new Shape();
            draw(fallback.graphics, 0, 0, a_icon, a_targetSize / 36, a_fill);
            fallback.filters = [new DropShadowFilter(1, 45, 0x000000, 0.6, 2, 2, 1, 1, false, false, false)];
            return fallback;
        }

        private static function symbolFor(a_icon:String):String
        {
            switch (a_icon)
            {
                case "ballistic": return "ballistic";
                case "ap":
                case "armor_piercing": return "armor_piercing";
                case "hollowPoint":
                case "hollow_point": return "hollow_point";
                case "subsonic": return "subsonic";
                case "incendiary": return "incendiary";
                case "explosive": return "explosive";
                case "shotgun": return "shotgun";
                case "energy":
                case "energy_ammo": return "energy_ammo";
                case "fusion_cell": return "fusion_cell";
                case "plasma": return "plasma";
                case "cryo": return "cryo";
                case "missile": return "missile";
                default: return "";
            }
        }

        public static function draw(a_graphics:Graphics, a_x:Number, a_y:Number, a_icon:String,
                                    a_scale:Number, a_fill:uint):void
        {
            if (!a_graphics || a_scale <= 0)
                return;

            const left:Number = a_x - 10 * a_scale;
            const right:Number = a_x + 10 * a_scale;
            const top:Number = a_y - 18 * a_scale;
            const bottom:Number = a_y + 18 * a_scale;
            const edge:uint = 0xFFF0A8;
            a_graphics.lineStyle(Math.max(0.8, 1.2 * a_scale), edge, 0.98, true);

            switch (a_icon)
            {
                case "shotgun":
                    // Shot shell: wide case, crimped mouth and a distinct base rim.
                    a_graphics.beginFill(a_fill, 1);
                    a_graphics.drawRoundRect(left + 1 * a_scale, top + 2 * a_scale,
                        18 * a_scale, 31 * a_scale, 4 * a_scale, 4 * a_scale);
                    a_graphics.endFill();
                    a_graphics.moveTo(left + 3 * a_scale, top + 5 * a_scale);
                    a_graphics.lineTo(right - 3 * a_scale, top + 5 * a_scale);
                    a_graphics.moveTo(left + 1 * a_scale, bottom - 4 * a_scale);
                    a_graphics.lineTo(right - 1 * a_scale, bottom - 4 * a_scale);
                    a_graphics.drawRect(a_x - 3 * a_scale, bottom - 2 * a_scale,
                        6 * a_scale, 2 * a_scale);
                    return;

                case "energy":
                    // Rectangular energy-cell casing with contact cap and charge bands.
                    a_graphics.beginFill(a_fill, 1);
                    a_graphics.drawRoundRect(left + 2 * a_scale, top + 5 * a_scale,
                        16 * a_scale, 27 * a_scale, 3 * a_scale, 3 * a_scale);
                    a_graphics.endFill();
                    a_graphics.beginFill(edge, 1);
                    a_graphics.drawRect(a_x - 3 * a_scale, top + 1 * a_scale,
                        6 * a_scale, 4 * a_scale);
                    a_graphics.endFill();
                    a_graphics.moveTo(left + 5 * a_scale, a_y - 3 * a_scale);
                    a_graphics.lineTo(right - 5 * a_scale, a_y - 3 * a_scale);
                    a_graphics.moveTo(left + 5 * a_scale, a_y + 4 * a_scale);
                    a_graphics.lineTo(right - 5 * a_scale, a_y + 4 * a_scale);
                    return;

                case "plasma":
                    a_graphics.beginFill(a_fill, 1);
                    a_graphics.drawCircle(a_x, a_y, 13 * a_scale);
                    a_graphics.endFill();
                    a_graphics.lineStyle(Math.max(1.1, 2 * a_scale), edge, 1, true);
                    a_graphics.drawCircle(a_x, a_y, 5 * a_scale);
                    a_graphics.moveTo(a_x - 3 * a_scale, a_y);
                    a_graphics.lineTo(a_x + 3 * a_scale, a_y);
                    return;

                case "cryo":
                    // Snowflake remains legible at sector size and is not confused with a bullet.
                    a_graphics.lineStyle(Math.max(1.3, 3 * a_scale), a_fill, 1, true);
                    for (var arm:int = 0; arm < 3; arm++)
                    {
                        const angle:Number = arm * Math.PI / 3;
                        const dx:Number = Math.cos(angle) * 15 * a_scale;
                        const dy:Number = Math.sin(angle) * 15 * a_scale;
                        a_graphics.moveTo(a_x - dx, a_y - dy);
                        a_graphics.lineTo(a_x + dx, a_y + dy);
                    }
                    return;

                case "explosive":
                    // Grenade projectile with a visible fuse cap.
                    a_graphics.beginFill(a_fill, 1);
                    a_graphics.moveTo(a_x, top + 3 * a_scale);
                    a_graphics.lineTo(right - 1 * a_scale, a_y - 7 * a_scale);
                    a_graphics.lineTo(right - 3 * a_scale, bottom - 2 * a_scale);
                    a_graphics.lineTo(left + 3 * a_scale, bottom - 2 * a_scale);
                    a_graphics.lineTo(left + 1 * a_scale, a_y - 7 * a_scale);
                    a_graphics.lineTo(a_x, top + 3 * a_scale);
                    a_graphics.endFill();
                    a_graphics.drawRect(a_x - 3 * a_scale, top, 6 * a_scale, 4 * a_scale);
                    a_graphics.moveTo(a_x + 4 * a_scale, top + 1 * a_scale);
                    a_graphics.lineTo(a_x + 8 * a_scale, top - 3 * a_scale);
                    return;

                case "incendiary":
                    a_graphics.beginFill(a_fill, 1);
                    a_graphics.moveTo(a_x, bottom - 1 * a_scale);
                    a_graphics.curveTo(left - 3 * a_scale, a_y + 5 * a_scale,
                        a_x - 4 * a_scale, a_y - 2 * a_scale);
                    a_graphics.curveTo(a_x - 1 * a_scale, top - 2 * a_scale,
                        a_x + 1 * a_scale, a_y - 7 * a_scale);
                    a_graphics.curveTo(right + 3 * a_scale, a_y + 1 * a_scale,
                        a_x + 5 * a_scale, a_y + 7 * a_scale);
                    a_graphics.curveTo(right, bottom, a_x, bottom - 1 * a_scale);
                    a_graphics.endFill();
                    return;

                case "ballistic":
                case "ap":
                case "hollowPoint":
                case "subsonic":
                default:
                    drawCartridge(a_graphics, a_x, a_y, a_icon, a_scale, a_fill, edge,
                        left, right, top, bottom);
                    return;
            }
        }

        private static function drawCartridge(a_graphics:Graphics, a_x:Number, a_y:Number,
                                              a_icon:String, a_scale:Number, a_fill:uint,
                                              a_edge:uint, a_left:Number, a_right:Number,
                                              a_top:Number, a_bottom:Number):void
        {
            const tipY:Number = a_y - 7 * a_scale;
            a_graphics.beginFill(a_fill, 1);
            a_graphics.moveTo(a_x, a_top);

            if (a_icon == "ap")
            {
                // Long pointed penetrator tip.
                a_graphics.lineTo(a_right, tipY);
                a_graphics.lineTo(a_x + 5 * a_scale, a_y + 7 * a_scale);
                a_graphics.lineTo(a_x + 5 * a_scale, a_bottom - 2 * a_scale);
                a_graphics.lineTo(a_x + 6 * a_scale, a_bottom);
                a_graphics.lineTo(a_x - 6 * a_scale, a_bottom);
                a_graphics.lineTo(a_x - 5 * a_scale, a_bottom - 2 * a_scale);
                a_graphics.lineTo(a_x - 5 * a_scale, a_y + 7 * a_scale);
                a_graphics.lineTo(a_left, tipY);
            }
            else if (a_icon == "hollowPoint")
            {
                // Open cavity at the nose, visible as a notched profile.
                a_graphics.lineTo(a_right, tipY);
                a_graphics.lineTo(a_right - 2 * a_scale, a_y - 2 * a_scale);
                a_graphics.lineTo(a_right - 6 * a_scale, a_y - 2 * a_scale);
                a_graphics.lineTo(a_right - 6 * a_scale, a_y + 2 * a_scale);
                a_graphics.lineTo(a_right - 2 * a_scale, a_y + 2 * a_scale);
                a_graphics.lineTo(a_right, a_y + 7 * a_scale);
                a_graphics.lineTo(a_x + 6 * a_scale, a_bottom - 2 * a_scale);
                a_graphics.lineTo(a_x + 6 * a_scale, a_bottom);
                a_graphics.lineTo(a_x - 6 * a_scale, a_bottom);
                a_graphics.lineTo(a_x - 6 * a_scale, a_bottom - 2 * a_scale);
                a_graphics.lineTo(a_left, a_y + 7 * a_scale);
                a_graphics.lineTo(a_left, tipY);
            }
            else
            {
                // Ballistic full-metal nose, or a short subsonic profile.
                if (a_icon == "subsonic")
                    a_graphics.lineTo(a_right - 2 * a_scale, a_y - 5 * a_scale);
                else
                    a_graphics.lineTo(a_right, tipY);
                a_graphics.lineTo(a_x + 6 * a_scale, a_bottom - 2 * a_scale);
                a_graphics.lineTo(a_x + 6 * a_scale, a_bottom);
                a_graphics.lineTo(a_x - 6 * a_scale, a_bottom);
                a_graphics.lineTo(a_x - 6 * a_scale, a_bottom - 2 * a_scale);
                a_graphics.lineTo(a_left, a_y + 7 * a_scale);
                a_graphics.lineTo(a_left, tipY);
            }

            a_graphics.lineTo(a_x, a_top);
            a_graphics.endFill();

            // Case mouth and extraction rim provide a common readable cartridge base.
            a_graphics.lineStyle(Math.max(0.7, 1.1 * a_scale), a_edge, 0.95, true);
            a_graphics.moveTo(a_left + 1 * a_scale, a_y + 9 * a_scale);
            a_graphics.lineTo(a_right - 1 * a_scale, a_y + 9 * a_scale);
            a_graphics.moveTo(a_left, a_bottom - 1 * a_scale);
            a_graphics.lineTo(a_right, a_bottom - 1 * a_scale);

            if (a_icon == "ap")
            {
                a_graphics.moveTo(a_x, a_top + 3 * a_scale);
                a_graphics.lineTo(a_x, tipY - 1 * a_scale);
            }
            else if (a_icon == "subsonic")
            {
                // Suppressor-like sound waves mark stealth ammunition.
                a_graphics.drawCircle(a_right + 4 * a_scale, a_y - 1 * a_scale, 2 * a_scale);
                a_graphics.drawCircle(a_right + 8 * a_scale, a_y - 1 * a_scale, 1.4 * a_scale);
            }
        }
    }
}
