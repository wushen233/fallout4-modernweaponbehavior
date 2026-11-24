package
{
    import flash.display.MovieClip;

    // GameMenuBase::SetUpButtonBar binds SetButtonHintData here and lets the
    // native ButtonBarMenu draw the wheel operation hint.
    public class AmmoWheelHintSource extends MovieClip
    {
        public var AmmoWheelDataSource:Boolean = true;
        public var bRedirectToButtonBarMenu:Boolean = true;
        public var bUseShadedBackground:Boolean = false;
        public var bAcquiredByNativeCode:Boolean = false;
        public var SetButtonHintData:Function;

        public function onAcquiredByNativeCode():void
        {
            bAcquiredByNativeCode = true;
        }
    }
}
